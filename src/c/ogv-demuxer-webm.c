#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>

#include <nestegg/nestegg.h>

#include "ogv-demuxer.h"
#include "ogv-buffer-queue.h"

static nestegg        *demuxContext;
static BufferQueue    *bufferQueue;

static bool            hasVideo = false;
static unsigned int    videoTrack = 0;
static int             videoCodec = -1;
static char           *videoCodecName = NULL;

static bool            hasAudio = false;
static unsigned int    audioTrack = 0;
static int             audioCodec = -1;
static char           *audioCodecName = NULL;

static int64_t         seekTime;
static unsigned int    seekTrack;

static double          lastKeyframeKimestamp = -1;

enum AppState {
    STATE_BEGIN,
    STATE_DECODING,
    STATE_SEEKING
} appState;

void ogv_demuxer_init() {
    appState = STATE_BEGIN;
    bufferQueue = bq_init();
}

static void logCallback(nestegg *context, unsigned int severity, char const * format, ...)
{
	if (severity >= NESTEGG_LOG_INFO) {
		va_list args;
		va_start(args, format);
		vprintf(format, args);
		va_end(args);
	}
}

static int readCallback(void * buffer, size_t length, void *userdata)
{
	if (bq_headroom((BufferQueue *)userdata) < length) {
		// End of stream. Demuxer can recover from this if more data comes in!
		return 0;
	}

	if (bq_read((BufferQueue *)userdata, buffer, length)) {
		// error
		return -1;
	} else {
		// success
		return 1;
	}
}

static int seekCallback(int64_t offset, int whence, void * userdata)
{
    int64_t pos;
    switch (whence) {
        case SEEK_SET:
            pos = offset;
            break;
        case SEEK_CUR:
            pos = ((BufferQueue *)userdata)->pos + offset;
            break;
        case SEEK_END: // not implemented
        default:
            return -1;
    }
    if (bq_seek((BufferQueue *)userdata, pos)) {
        //printf("Buffer seek failure in webm demuxer\n");
        return -1;
    } else {
        return 0;
    }
}

static int64_t tellCallback(void * userdata)
{
    return bq_tell((BufferQueue *)userdata);
}

static nestegg_io ioCallbacks = {
	readCallback,
	seekCallback,
	tellCallback,
	NULL
};

/**
 * Safe read of EBML id or data size int64 from a data stream.
 * @returns byte count of the ebml number on success, or 0 on failure
 */
static int read_ebml_int64(BufferQueue *bufferQueue, int64_t *val, int keep_mask_bit)
{
    // Count of initial 0 bits plus first 1 bit encode total number of bytes.
    // Rest of the bits are a big-endian number.
    char first;
    if (bq_read(bufferQueue, &first, 1)) {
        //printf("out of bytes at start of field\n");
        return 0;
    }
    if (first == 0) {
        //printf("zero field\n");
        return 0;
    }

    int shift = 0;
    while (((unsigned char)first & 0x80) == 0) {
        shift++;
        first = (unsigned char)first << 1;
    }
    int byteCount = shift + 1;

    if (!keep_mask_bit) {
        // id keeps the mask bit, data size strips it
        first = first & 0x7f;
    }
    // Save the top bits from that first byte.
    *val = (unsigned char)first >> shift;
    for (int i = 1; i < byteCount; i++) {
        char next;
        if (bq_read(bufferQueue, &next, 1)) {
            //printf("out of bytes in field\n");
            return 0;
        }
        *val = *val << 8 | (unsigned char)next;
    }
    //printf("byteCount %d; val %lld\n", byteCount, *val);
    return byteCount;
}

static int readyForNextPacket()
{
    int64_t pos = bq_tell(bufferQueue);
    int ok = 0;
    
    int64_t id, size;
    int idSize, sizeSize;
    
    idSize = read_ebml_int64(bufferQueue, &id, 1);
    if (idSize) {
        if (id != 0x1c53bb6bLL) {
            // Right now we only care about reading the cues.
            // If used elsewhere, unpack that. ;)
            ok = 1;
        }
        sizeSize = read_ebml_int64(bufferQueue, &size, 0);
        if (sizeSize) {
            //printf("packet is %llx, size is %lld, headroom %lld\n", id, size, bq_headroom(bufferQueue));
            if (bq_headroom(bufferQueue) >= size) {
                ok = 1;
            }
        }
    }
    /*
    if (!ok) {
        printf("not ready for packet! %lld/%lld %d %d %llx %lld\n", bq_tell(bufferQueue), bq_headroom(bufferQueue), idSize, sizeSize, id, size);
    } else {
        printf("ready for packet! %lld/%lld %d %d %llx %lld\n", bq_tell(bufferQueue), bq_headroom(bufferQueue), idSize, sizeSize, id, size);
    }
    */
    bq_seek(bufferQueue, pos);
    return ok;
}

static int processBegin() {
	// This will read through headers, hopefully we have enough data
	// or else it may fail and explode.
	// @todo rework all this to faux sync or else full async
    ioCallbacks.userdata = (void *)bufferQueue;
	if (nestegg_init(&demuxContext, ioCallbacks, logCallback, -1) < 0) {
		return 0;
	}

	// Look through the tracks finding our video and audio
	unsigned int tracks;
	if (nestegg_track_count(demuxContext, &tracks) < 0) {
		tracks = 0;
	}
	for (unsigned int track = 0; track < tracks; track++) {
		int trackType = nestegg_track_type(demuxContext, track);
		int codec = nestegg_track_codec_id(demuxContext, track);
		
		if (trackType == NESTEGG_TRACK_VIDEO && !hasVideo) {
			if (codec == NESTEGG_CODEC_VP8) {
				hasVideo = 1;
				videoTrack = track;
				videoCodec = codec;
				videoCodecName = "vp8";
			}
			if (codec == NESTEGG_CODEC_VP9) {
				hasVideo = 1;
				videoTrack = track;
				videoCodec = codec;
				videoCodecName = "vp9";
			}
		}
		
		if (trackType == NESTEGG_TRACK_AUDIO && !hasAudio) {
			if (codec == NESTEGG_CODEC_VORBIS) {
				hasAudio = 1;
				audioTrack = track;
				audioCodec = codec;
				audioCodecName = "vorbis";
			}
			if (codec == NESTEGG_CODEC_OPUS) {
				hasAudio = 1;
				audioTrack = track;
				audioCodec = codec;
				audioCodecName = "opus";
			}
		}
	}

	if (hasVideo) {
		nestegg_video_params videoParams;
		if (nestegg_track_video_params(demuxContext, videoTrack, &videoParams) < 0) {
			// failed! something is wrong...
			hasVideo = 0;
		} else {
			ogvjs_callback_init_video(videoParams.width, videoParams.height,
			                          videoParams.width >> 1, videoParams.height >> 1, // @todo assuming 4:2:0
			                          0, // @todo get fps
			                          videoParams.width - videoParams.crop_left - videoParams.crop_right,
			                          videoParams.height - videoParams.crop_top - videoParams.crop_bottom,
                                      videoParams.crop_left, videoParams.crop_top,
                                      videoParams.display_width, videoParams.display_height);
		}
	}

	if (hasAudio) {
		nestegg_audio_params audioParams;
		if (nestegg_track_audio_params(demuxContext, audioTrack, &audioParams) < 0) {
			// failed! something is wrong
			hasAudio = 0;
		} else {
			unsigned int codecDataCount;
			nestegg_track_codec_data_count(demuxContext, audioTrack, &codecDataCount);
			
            for (unsigned int i = 0; i < codecDataCount; i++) {
                unsigned char *data;
                size_t len;
                int ret = nestegg_track_codec_data(demuxContext, audioTrack, i, &data, &len);
                if (ret < 0) {
                	//printf("failed to read codec data %d\n", i);
                	abort();
                }
                // ... store these!
                ogvjs_callback_audio_packet((char *)data, len, -1);
			}
		}
	}

	appState = STATE_DECODING;
	ogvjs_callback_loaded_metadata(videoCodecName, audioCodecName);

	return 1;
}

static int packet_is_keyframe_vp8(const unsigned char *data, size_t data_len) {
	return (data_len > 0 && ((data[0] & 1) == 0));
}

static int big_endian_bit(unsigned char val, int index) {
  return (val << index) & 0x80 ? 1 : 0;
}

static int packet_is_keyframe_vp9(const unsigned char *data, size_t data_len) {
  if (data_len == 0) {
    return 0;
  }

  int shift = 0;
  int frame_marker_high = big_endian_bit(data[0], shift++);
  int frame_marker_low = big_endian_bit(data[0], shift++);
  int frame_marker = (frame_marker_high << 1) + frame_marker_low;
  if (frame_marker != 2) {
    // invalid frame?
    return 0;
  }
  int profile_high = big_endian_bit(data[0], shift++);
  int profile_low = big_endian_bit(data[0], shift++);
  int profile = (profile_high << 1) + profile_low;
  if (profile == 3) {
    // reserved 0
    shift++;
  }
  int show_existing_frame = big_endian_bit(data[0], shift++);
  if (show_existing_frame) {
    return 0;
  }

  int frame_type = big_endian_bit(data[0], shift++);
  return (frame_type == 0);
}

static int processDecoding() {
	//printf("webm processDecoding: reading next packet...\n");

	// Do the nestegg_read_packet dance until it fails to read more data,
	// at which point we ask for more. Hope it doesn't explode.
	nestegg_packet *packet = NULL;
	int ret = nestegg_read_packet(demuxContext, &packet);
	if (ret == 0) {
		// End of stream? Usually means we need more data.
		nestegg_read_reset(demuxContext);
		return 0;
	} else if (ret < 0) {
		// Unknown unrecoverable error
		//printf("webm processDecoding: error %d\n", ret);
		return 0;
	} else {
		//printf("webm processDecoding: got packet?\n");
		unsigned int track;
		nestegg_packet_track(packet, &track);

		uint64_t timestamp_ns;
		nestegg_packet_tstamp(packet, &timestamp_ns);
		float timestamp = timestamp_ns / 1000000000.0;

		unsigned char *data = NULL;
		size_t data_len = 0;
		nestegg_packet_data(packet, 0, &data, &data_len);

		if (hasVideo && track == videoTrack) {
      int isKeyframe;
      if ((videoCodec == NESTEGG_CODEC_VP8 && packet_is_keyframe_vp8(data, data_len)) ||
          (videoCodec == NESTEGG_CODEC_VP9 && packet_is_keyframe_vp9(data, data_len))) {
        lastKeyframeKimestamp = timestamp;
        isKeyframe = 1;
      } else {
        isKeyframe = 0;
      }
			ogvjs_callback_video_packet((char *)data, data_len, timestamp, lastKeyframeKimestamp, isKeyframe);
		} else if (hasAudio && track == audioTrack) {
			ogvjs_callback_audio_packet((char *)data, data_len, timestamp);
		} else {
			// throw away unknown packets
		}

		nestegg_free_packet(packet);
		return 1;
	}
}

static int processSeeking()
{
    bufferQueue->lastSeekTarget = 0;
    int r = nestegg_track_seek(demuxContext, seekTrack, seekTime);
    if (r) {
        if (bufferQueue->lastSeekTarget == 0) {
            // Maybe we just need more data?
            //printf("is seeking processing... FAILED at %lld %lld %lld\n", bufferQueue->pos, bq_start(bufferQueue), bq_end(bufferQueue));
        } else {
            // We need to go off and load stuff...
            //printf("is seeking processing... MOAR SEEK %lld %lld %lld\n", bufferQueue->lastSeekTarget, bq_start(bufferQueue), bq_end(bufferQueue));
            int64_t target = bufferQueue->lastSeekTarget;
            bq_flush(bufferQueue);
            bufferQueue->pos = target;
            ogvjs_callback_seek(target);
        }
        return 0;
    } else {
        appState = STATE_DECODING;
        //printf("is seeking processing... LOOKS ROLL OVER\n");
        return 0;
    }
}

void ogv_demuxer_receive_input(const char *buffer, int bufsize) {
    if (bufsize > 0) {
        bq_append(bufferQueue, buffer, bufsize);
    }
}

int ogv_demuxer_process() {
	if (appState == STATE_BEGIN) {
        if (bq_headroom(bufferQueue) > 256 * 1024) {
            return processBegin();
        } else {
            // need more data
            return 0;
        }
    } else if (appState == STATE_DECODING) {
        return processDecoding();
    } else if (appState == STATE_SEEKING) {
        if (readyForNextPacket()) {
            return processSeeking();
        } else {
            // need more data
            //printf("not ready to read the cues\n");
            return 0;
        }
	} else {
		// uhhh...
		//printf("Invalid appState in ogv_demuxer_process\n");
        return 0;
	}
}

void ogv_demuxer_destroy() {
	// should probably tear stuff down, eh
    bq_free(bufferQueue);
    bufferQueue = NULL;
}

void ogv_demuxer_flush() {
    bq_flush(bufferQueue);
    // we may not need to handle the packet queue because this only
    // happens after seeking and nestegg handles that internally
    lastKeyframeKimestamp = -1;
}

/**
 * @return segment length in bytes, or -1 if unknown
 */
long ogv_demuxer_media_length() {
	// @todo check if this is needed? maybe an ogg-specific thing
	return -1;
}

/**
 * @return segment duration in seconds, or -1 if unknown
 */
float ogv_demuxer_media_duration() {
	uint64_t duration_ns;
    if (nestegg_duration(demuxContext, &duration_ns) < 0) {
    	return -1;
    } else {
	    return duration_ns / 1000000000.0;
	}
}

int ogv_demuxer_seekable()
{
	return nestegg_has_cues(demuxContext);
}

long ogv_demuxer_keypoint_offset(long time_ms)
{
	// can't do with nestegg's API; use ogv_demuxer_seek_to_keypoint instead
	return -1;
}

int ogv_demuxer_seek_to_keypoint(long time_ms)
{
    appState = STATE_SEEKING;
    seekTime = (int64_t)time_ms * 1000000LL;
    if (hasVideo) {
        seekTrack = videoTrack;
    } else if (hasAudio) {
        seekTrack = audioTrack;
    } else {
        return 0;
    }
    processSeeking();
    return 1;
}
