/* Lua libs */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#if LUA_VERSION_NUM < 502
# define luaL_newlib(L,l) (lua_newtable(L), luaL_register(L,NULL,l))
#endif

void luaL_setfuncs (lua_State *L, const luaL_Reg *l, int nup) {
  luaL_checkstack(L, nup, "too many upvalues");
  for (; l->name != NULL; l++) {  /* fill the table with given functions */
    int i;
    for (i = 0; i < nup; i++)  /* copy upvalues to the top */
      lua_pushvalue(L, -nup);
    lua_pushcclosure(L, l->func, nup);  /* closure with those upvalues */
    lua_setfield(L, -(nup + 2), l->name);
  }
  lua_pop(L, nup);  /* remove upvalues */
}

/* Torch libs */
#include <luaT.h>
#include <TH/TH.h>

/* FFmpeg libs */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/avcodec.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>

#include <string.h>

typedef struct {
  AVFrame *frame;
} VideoFrame;

static int VideoFrame_to_byte_tensor(lua_State *L) {
  VideoFrame *self = (VideoFrame*)luaL_checkudata(L, 1, "VideoFrame");

  THByteStorage *storage;
  THByteTensor *tensor;

  const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(self->frame->format);
  int is_packed_rgb = (desc->flags & (AV_PIX_FMT_FLAG_PLANAR | AV_PIX_FMT_FLAG_RGB)) == AV_PIX_FMT_FLAG_RGB;

  if(!is_packed_rgb) {
    // Calculate number of channels (eg 3 for YUV, 1 for greyscale)
    int n_channels;
    for(n_channels = 4;
      n_channels > 0 && self->frame->linesize[n_channels - 1] == 0;
      --n_channels);

    storage = THByteStorage_newWithSize(
      n_channels * self->frame->height * self->frame->width);
    tensor = THByteTensor_newWithStorage3d(storage, 0,
      n_channels, self->frame->height * self->frame->width,
      self->frame->height, self->frame->width,
      self->frame->width, 1);

    unsigned char *ptr = storage->data;
    int i, y, x;
    for(i = 0; i < n_channels; ++i) {
      int stride = self->frame->linesize[i];
      unsigned char *channel_data = self->frame->data[i];
      for(y = 0; y < self->frame->height; ++y) {
        int offset = stride * y;
        for(x = 0; x < self->frame->width; ++x) {
          *ptr++ = channel_data[offset + x];
        }
      }
    }
  } else {
    if(self->frame->format != PIX_FMT_RGB24) {
      return luaL_error(L, "rgb24 is the only supported packed RGB format");
    }

    storage = THByteStorage_newWithSize(
      3 * self->frame->height * self->frame->width);
    tensor = THByteTensor_newWithStorage3d(storage, 0,
      3, self->frame->height * self->frame->width,
      self->frame->height, self->frame->width,
      self->frame->width, 1);

    unsigned char *ptr = storage->data;
    int triple_x_max = self->frame->width * 3;
    int i, y, triple_x;
    for(i = 0; i < 3; ++i) {
      for(y = 0; y < self->frame->height; ++y) {
        int offset = y * self->frame->linesize[0] + i;
        for(triple_x = 0; triple_x < triple_x_max; triple_x += 3) {
          *ptr++ = self->frame->data[0][offset + triple_x];
        }
      }
    }
  }

  luaT_pushudata(L, tensor, "torch.ByteTensor");

  return 1;
}

static const luaL_Reg VideoFrame_functions[] = {
  {NULL, NULL}
};

static const luaL_Reg VideoFrame_methods[] = {
  {"to_byte_tensor", VideoFrame_to_byte_tensor},
  {NULL, NULL}
};

static void register_VideoFrame(lua_State *L, int m) {
  luaL_newmetatable(L, "VideoFrame");
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");

  luaL_setfuncs(L, VideoFrame_methods, 0);
  lua_pop(L, 1);

  luaL_newlib(L, VideoFrame_functions);

  lua_setfield(L, m, "VideoFrame");
}

typedef struct {
  AVFormatContext *format_context;
  int video_stream_index;
  AVCodecContext *video_decoder_context;
  AVPacket packet;
  AVFrame *frame;
  AVFilterGraph *filter_graph;
  AVFilterContext *buffersrc_context;
  AVFilterContext *buffersink_context;
  AVFrame *filtered_frame;
} Video;

static int Video_new(lua_State *L) {
  if(lua_gettop(L) != 1) {
    return luaL_error(L, "invalid number of arguments: <path> expected");
  }

  const char *path = luaL_checkstring(L, 1);

  Video *self = lua_newuserdata(L, sizeof(Video));
  memset(self, 0, sizeof(Video));

  if(avformat_open_input(&self->format_context, path, NULL, NULL) < 0) {
    return luaL_error(L, "failed to open video input for %s", path);
  }

  if(avformat_find_stream_info(self->format_context, NULL) < 0) {
    return luaL_error(L, "failed to find stream info for %s", path);
  }

  AVCodec *decoder;
  self->video_stream_index = av_find_best_stream(self->format_context,
    AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
  if(self->video_stream_index < 0) {
    return luaL_error(L, "failed to find video stream for %s", path);
  }

  self->video_decoder_context = self->format_context->streams[self->video_stream_index]->codec;

  if(avcodec_open2(self->video_decoder_context, decoder, NULL) < 0) {
    return luaL_error(L, "failed to open video decoder for %s", path);
  }

  // av_dump_format(self->format_context, 0, path, 0);

  self->frame = av_frame_alloc();

  luaL_getmetatable(L, "Video");
  lua_setmetatable(L, -2);

  return 1;
}

static int Video_duration(lua_State *L) {
  Video *self = (Video*)luaL_checkudata(L, 1, "Video");

  lua_Number duration_in_seconds =
    (lua_Number)self->format_context->duration / AV_TIME_BASE;

  lua_pushnumber(L, duration_in_seconds);

  return 1;
}

static int Video_filter(lua_State *L) {
  int n_args = lua_gettop(L);

  Video *self = (Video*)luaL_checkudata(L, 1, "Video");
  const char *pixel_format_name = luaL_checkstring(L, 2);
  const char *filterchain;
  if(n_args < 3) {
    filterchain = "null";
  } else {
    filterchain = luaL_checkstring(L, 3);
  }

  if(self->filter_graph) {
    return luaL_error(L, "filter already set for this video");
  }

  const char* error_msg = 0;

  AVFilter *buffersrc = avfilter_get_by_name("buffer");
  AVFilter *buffersink = avfilter_get_by_name("buffersink");
  AVFilterInOut *outputs = avfilter_inout_alloc();
  AVFilterInOut *inputs = avfilter_inout_alloc();
  AVFilterGraph *filter_graph = avfilter_graph_alloc();

  char in_args[512];
  snprintf(in_args, sizeof(in_args),
    "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
    self->video_decoder_context->width,
    self->video_decoder_context->height,
    self->video_decoder_context->pix_fmt,
    self->video_decoder_context->time_base.num,
    self->video_decoder_context->time_base.den,
    self->video_decoder_context->sample_aspect_ratio.num,
    self->video_decoder_context->sample_aspect_ratio.den);

  AVFilterContext *buffersrc_context;
  AVFilterContext *buffersink_context;

  if(avfilter_graph_create_filter(&buffersrc_context, buffersrc, "in",
    in_args, NULL, filter_graph) < 0)
  {
    error_msg = "cannot create buffer source";
    goto end;
  }

  if(avfilter_graph_create_filter(&buffersink_context, buffersink, "out",
    NULL, NULL, filter_graph) < 0)
  {
    error_msg = "cannot create buffer sink";
    goto end;
  }

  enum AVPixelFormat pix_fmt = av_get_pix_fmt(pixel_format_name);
  if(pix_fmt == AV_PIX_FMT_NONE) {
    error_msg = "invalid pixel format name";
    goto end;
  }
  if(av_opt_set_bin(buffersink_context, "pix_fmts",
    (const unsigned char*)&pix_fmt, sizeof(enum AVPixelFormat),
    AV_OPT_SEARCH_CHILDREN) < 0)
  {
    error_msg = "failed to set output pixel format";
    goto end;
  }

  outputs->name       = av_strdup("in");
  outputs->filter_ctx = buffersrc_context;
  outputs->pad_idx    = 0;
  outputs->next       = NULL;

  inputs->name       = av_strdup("out");
  inputs->filter_ctx = buffersink_context;
  inputs->pad_idx    = 0;
  inputs->next       = NULL;

  if(avfilter_graph_parse_ptr(filter_graph, filterchain,
    &inputs, &outputs, NULL) < 0)
  {
    error_msg = "failed to parse filterchain description";
    goto end;
  }

  if(avfilter_graph_config(filter_graph, NULL) < 0) {
    error_msg = "failed to configure filter graph";
    goto end;
  }

  Video *filtered_video = lua_newuserdata(L, sizeof(Video));
  *filtered_video = *self;

  filtered_video->filter_graph = filter_graph;
  filtered_video->buffersrc_context = buffersrc_context;
  filtered_video->buffersink_context = buffersink_context;
  filtered_video->filtered_frame = av_frame_alloc();

  luaL_getmetatable(L, "Video");
  lua_setmetatable(L, -2);

end:
  avfilter_inout_free(&inputs);
  avfilter_inout_free(&outputs);

  if(error_msg) return luaL_error(L, error_msg);

  return 1;
}

static int Video_next_video_frame(lua_State *L) {
  Video *self = (Video*)luaL_checkudata(L, 1, "Video");

  VideoFrame *video_frame = lua_newuserdata(L, sizeof(VideoFrame));

  if(self->filter_graph && av_buffersink_get_frame(self->buffersink_context,
    self->filtered_frame) >= 0)
  {
    video_frame->frame = self->filtered_frame;
  } else {
    int found_video_frame;

read_video_frame:
    found_video_frame = 0;

    while(!found_video_frame) {
      if(av_read_frame(self->format_context, &self->packet) != 0) {
        return luaL_error(L, "couldn't read next frame");
      }

      if(self->packet.stream_index == self->video_stream_index) {
        av_frame_unref(self->frame);

        if(avcodec_decode_video2(self->video_decoder_context, self->frame,
          &found_video_frame, &self->packet) < 0)
        {
          return luaL_error(L, "couldn't decode video frame");
        }
      }
    }

    if(self->filter_graph) {
      // Push the decoded frame into the filtergraph
      if(av_buffersrc_add_frame_flags(self->buffersrc_context,
        self->frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
      {
        return luaL_error(L, "error while feeding the filtergraph");
      }

      // Pull filtered frames from the filtergraph
      av_frame_unref(self->filtered_frame);
      if(av_buffersink_get_frame(self->buffersink_context, self->filtered_frame) < 0) {
        goto read_video_frame;
      }
      video_frame->frame = self->filtered_frame;
    } else {
      video_frame->frame = self->frame;
    }
  }

  luaL_getmetatable(L, "VideoFrame");
  lua_setmetatable(L, -2);

  return 1;
}

static int Video_destroy(lua_State *L) {
  Video *self = (Video*)luaL_checkudata(L, 1, "Video");

  avformat_close_input(&self->format_context);
  avcodec_close(self->video_decoder_context);

  av_packet_unref(&self->packet);

  if(self->frame != NULL) {
    av_frame_unref(self->frame);
    av_frame_free(&self->frame);
  }

  if(self->filter_graph) {
    avfilter_graph_free(&self->filter_graph);
  }

  if(self->filtered_frame != NULL) {
    av_frame_unref(self->filtered_frame);
    av_frame_free(&self->filtered_frame);
  }

  return 0;
}

static const luaL_Reg Video_functions[] = {
  {"new", Video_new},
  {NULL, NULL}
};

static const luaL_Reg Video_methods[] = {
  {"duration", Video_duration},
  {"filter", Video_filter},
  {"next_video_frame", Video_next_video_frame},
  {"__gc", Video_destroy},
  {NULL, NULL}
};

static void register_Video(lua_State *L, int m) {
  luaL_newmetatable(L, "Video");
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");

  luaL_setfuncs(L, Video_methods, 0);
  lua_pop(L, 1);

  luaL_newlib(L, Video_functions);

  lua_setfield(L, m, "Video");
}

int luaopen_torchvid(lua_State *L) {
  // Initialization
  av_log_set_level(AV_LOG_ERROR);
  avcodec_register_all();
  av_register_all();
  avfilter_register_all();

  // Create table for module
  lua_newtable(L);
  int m = lua_gettop(L);

  // Add values for classes
  register_Video(L, m);
  register_VideoFrame(L, m);

  return 1;
}