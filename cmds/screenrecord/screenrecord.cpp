/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "ScreenRecord"
//#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <binder/IPCThreadState.h>
#include <utils/Errors.h>
#include <utils/Thread.h>

#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <ui/DisplayInfo.h>
#include <media/openmax/OMX_IVCommon.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaMuxer.h>
#include <media/ICrypto.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <getopt.h>

using namespace android;

// Command-line parameters.
static bool gVerbose = false;               // chatty on stdout
static bool gRotate = false;                // rotate 90 degrees
static bool gSizeSpecified = false;         // was size explicitly requested?
static uint32_t gVideoWidth = 0;            // default width+height
static uint32_t gVideoHeight = 0;
static uint32_t gBitRate = 4000000;         // 4Mbps

// Set by signal handler to stop recording.
static bool gStopRequested;

// Previous signal handler state, restored after first hit.
static struct sigaction gOrigSigactionINT;
static struct sigaction gOrigSigactionHUP;

static const uint32_t kMinBitRate = 100000;         // 0.1Mbps
static const uint32_t kMaxBitRate = 100 * 1000000;  // 100Mbps

/*
 * Catch keyboard interrupt signals.  On receipt, the "stop requested"
 * flag is raised, and the original handler is restored (so that, if
 * we get stuck finishing, a second Ctrl-C will kill the process).
 */
static void signalCatcher(int signum)
{
    gStopRequested = true;
    switch (signum) {
    case SIGINT:
        sigaction(SIGINT, &gOrigSigactionINT, NULL);
        break;
    case SIGHUP:
        sigaction(SIGHUP, &gOrigSigactionHUP, NULL);
        break;
    default:
        abort();
        break;
    }
}

/*
 * Configures signal handlers.  The previous handlers are saved.
 *
 * If the command is run from an interactive adb shell, we get SIGINT
 * when Ctrl-C is hit.  If we're run from the host, the local adb process
 * gets the signal, and we get a SIGHUP when the terminal disconnects.
 */
static status_t configureSignals()
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signalCatcher;
    if (sigaction(SIGINT, &act, &gOrigSigactionINT) != 0) {
        status_t err = -errno;
        fprintf(stderr, "Unable to configure SIGINT handler: %s\n",
                strerror(errno));
        return err;
    }
    if (sigaction(SIGHUP, &act, &gOrigSigactionHUP) != 0) {
        status_t err = -errno;
        fprintf(stderr, "Unable to configure SIGHUP handler: %s\n",
                strerror(errno));
        return err;
    }
    return NO_ERROR;
}

/*
 * Returns "true" if the device is rotated 90 degrees.
 */
static bool isDeviceRotated(int orientation) {
    return orientation != DISPLAY_ORIENTATION_0 &&
            orientation != DISPLAY_ORIENTATION_180;
}

/*
 * Configures and starts the MediaCodec encoder.  Obtains an input surface
 * from the codec.
 */
static status_t prepareEncoder(float displayFps, sp<MediaCodec>* pCodec,
        sp<IGraphicBufferProducer>* pBufferProducer) {
    status_t err;

    if (gVerbose) {
        printf("Configuring recorder for %dx%d video at %.2fMbps\n",
                gVideoWidth, gVideoHeight, gBitRate / 1000000.0);
    }

    sp<AMessage> format = new AMessage;
    format->setInt32("width", gVideoWidth);
    format->setInt32("height", gVideoHeight);
    format->setString("mime", "video/avc");
    format->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);
    format->setInt32("bitrate", gBitRate);
    format->setFloat("frame-rate", displayFps);
    format->setInt32("i-frame-interval", 10);

    // MediaCodec
    sp<ALooper> looper = new ALooper;
    looper->setName("screenrecord_looper");
    looper->start();
    ALOGV("Creating codec");
    sp<MediaCodec> codec = MediaCodec::CreateByType(looper, "video/avc", true);
    if (codec == NULL) {
        fprintf(stderr, "ERROR: unable to create video/avc codec instance\n");
        return UNKNOWN_ERROR;
    }
    err = codec->configure(format, NULL, NULL,
            MediaCodec::CONFIGURE_FLAG_ENCODE);
    if (err != NO_ERROR) {
        fprintf(stderr, "ERROR: unable to configure codec (err=%d)\n", err);
        return err;
    }

    ALOGV("Creating buffer producer");
    sp<IGraphicBufferProducer> bufferProducer;
    err = codec->createInputSurface(&bufferProducer);
    if (err != NO_ERROR) {
        fprintf(stderr,
            "ERROR: unable to create encoder input surface (err=%d)\n", err);
        return err;
    }

    ALOGV("Starting codec");
    err = codec->start();
    if (err != NO_ERROR) {
        fprintf(stderr, "ERROR: unable to start codec (err=%d)\n", err);
        return err;
    }

    ALOGV("Codec prepared");
    *pCodec = codec;
    *pBufferProducer = bufferProducer;
    return 0;
}

/*
 * Configures the virtual display.  When this completes, virtual display
 * frames will start being sent to the encoder's surface.
 */
static status_t prepareVirtualDisplay(const DisplayInfo& mainDpyInfo,
        const sp<IGraphicBufferProducer>& bufferProducer,
        sp<IBinder>* pDisplayHandle) {
    status_t err;

    // Set the region of the layer stack we're interested in, which in our
    // case is "all of it".  If the app is rotated (so that the width of the
    // app is based on the height of the display), reverse width/height.
    bool deviceRotated = isDeviceRotated(mainDpyInfo.orientation);
    uint32_t sourceWidth, sourceHeight;
    if (!deviceRotated) {
        sourceWidth = mainDpyInfo.w;
        sourceHeight = mainDpyInfo.h;
    } else {
        ALOGV("using rotated width/height");
        sourceHeight = mainDpyInfo.w;
        sourceWidth = mainDpyInfo.h;
    }
    Rect layerStackRect(sourceWidth, sourceHeight);

    // We need to preserve the aspect ratio of the display.
    float displayAspect = (float) sourceHeight / (float) sourceWidth;


    // Set the way we map the output onto the display surface (which will
    // be e.g. 1280x720 for a 720p video).  The rect is interpreted
    // post-rotation, so if the display is rotated 90 degrees we need to
    // "pre-rotate" it by flipping width/height, so that the orientation
    // adjustment changes it back.
    //
    // We might want to encode a portrait display as landscape to use more
    // of the screen real estate.  (If players respect a 90-degree rotation
    // hint, we can essentially get a 720x1280 video instead of 1280x720.)
    // In that case, we swap the configured video width/height and then
    // supply a rotation value to the display projection.
    uint32_t videoWidth, videoHeight;
    uint32_t outWidth, outHeight;
    if (!gRotate) {
        videoWidth = gVideoWidth;
        videoHeight = gVideoHeight;
    } else {
        videoWidth = gVideoHeight;
        videoHeight = gVideoWidth;
    }
    if (videoHeight > (uint32_t)(videoWidth * displayAspect)) {
        // limited by narrow width; reduce height
        outWidth = videoWidth;
        outHeight = (uint32_t)(videoWidth * displayAspect);
    } else {
        // limited by short height; restrict width
        outHeight = videoHeight;
        outWidth = (uint32_t)(videoHeight / displayAspect);
    }
    uint32_t offX, offY;
    offX = (videoWidth - outWidth) / 2;
    offY = (videoHeight - outHeight) / 2;
    Rect displayRect(offX, offY, offX + outWidth, offY + outHeight);

    if (gVerbose) {
        if (gRotate) {
            printf("Rotated content area is %ux%u at offset x=%d y=%d\n",
                    outHeight, outWidth, offY, offX);
        } else {
            printf("Content area is %ux%u at offset x=%d y=%d\n",
                    outWidth, outHeight, offX, offY);
        }
    }


    sp<IBinder> dpy = SurfaceComposerClient::createDisplay(
            String8("ScreenRecorder"), false /* secure */);

    SurfaceComposerClient::openGlobalTransaction();
    SurfaceComposerClient::setDisplaySurface(dpy, bufferProducer);
    SurfaceComposerClient::setDisplayProjection(dpy,
            gRotate ? DISPLAY_ORIENTATION_90 : DISPLAY_ORIENTATION_0,
            layerStackRect, displayRect);
    SurfaceComposerClient::setDisplayLayerStack(dpy, 0);    // default stack
    SurfaceComposerClient::closeGlobalTransaction();

    *pDisplayHandle = dpy;

    return NO_ERROR;
}

/*
 * Runs the MediaCodec encoder, sending the output to the MediaMuxer.  The
 * input frames are coming from the virtual display as fast as SurfaceFlinger
 * wants to send them.
 *
 * The muxer must *not* have been started before calling.
 */
static status_t runEncoder(const sp<MediaCodec>& encoder,
        const sp<MediaMuxer>& muxer) {
    static int kTimeout = 250000;   // be responsive on signal
    status_t err;
    ssize_t trackIdx = -1;
    uint32_t debugNumFrames = 0;
    time_t debugStartWhen = time(NULL);

    Vector<sp<ABuffer> > buffers;
    err = encoder->getOutputBuffers(&buffers);
    if (err != NO_ERROR) {
        fprintf(stderr, "Unable to get output buffers (err=%d)\n", err);
        return err;
    }

    // This is set by the signal handler.
    gStopRequested = false;

    // Run until we're signaled.
    while (!gStopRequested) {
        size_t bufIndex, offset, size;
        int64_t ptsUsec;
        uint32_t flags;
        ALOGV("Calling dequeueOutputBuffer");
        err = encoder->dequeueOutputBuffer(&bufIndex, &offset, &size, &ptsUsec,
                &flags, kTimeout);
        ALOGV("dequeueOutputBuffer returned %d", err);
        switch (err) {
        case NO_ERROR:
            // got a buffer
            if ((flags & MediaCodec::BUFFER_FLAG_CODECCONFIG) != 0) {
                // ignore this -- we passed the CSD into MediaMuxer when
                // we got the format change notification
                ALOGV("Got codec config buffer (%u bytes); ignoring", size);
                size = 0;
            }
            if (size != 0) {
                ALOGV("Got data in buffer %d, size=%d, pts=%lld",
                        bufIndex, size, ptsUsec);
                CHECK(trackIdx != -1);

                // If the virtual display isn't providing us with timestamps,
                // use the current time.
                if (ptsUsec == 0) {
                    ptsUsec = systemTime(SYSTEM_TIME_MONOTONIC) / 1000;
                }

                // The MediaMuxer docs are unclear, but it appears that we
                // need to pass either the full set of BufferInfo flags, or
                // (flags & BUFFER_FLAG_SYNCFRAME).
                err = muxer->writeSampleData(buffers[bufIndex], trackIdx,
                        ptsUsec, flags);
                if (err != NO_ERROR) {
                    fprintf(stderr, "Failed writing data to muxer (err=%d)\n",
                            err);
                    return err;
                }
                debugNumFrames++;
            }
            err = encoder->releaseOutputBuffer(bufIndex);
            if (err != NO_ERROR) {
                fprintf(stderr, "Unable to release output buffer (err=%d)\n",
                        err);
                return err;
            }
            if ((flags & MediaCodec::BUFFER_FLAG_EOS) != 0) {
                // Not expecting EOS from SurfaceFlinger.  Go with it.
                ALOGD("Received end-of-stream");
                gStopRequested = false;
            }
            break;
        case -EAGAIN:                       // INFO_TRY_AGAIN_LATER
            // not expected with infinite timeout
            ALOGV("Got -EAGAIN, looping");
            break;
        case INFO_FORMAT_CHANGED:           // INFO_OUTPUT_FORMAT_CHANGED
            {
                // format includes CSD, which we must provide to muxer
                ALOGV("Encoder format changed");
                sp<AMessage> newFormat;
                encoder->getOutputFormat(&newFormat);
                trackIdx = muxer->addTrack(newFormat);
                ALOGV("Starting muxer");
                err = muxer->start();
                if (err != NO_ERROR) {
                    fprintf(stderr, "Unable to start muxer (err=%d)\n", err);
                    return err;
                }
            }
            break;
        case INFO_OUTPUT_BUFFERS_CHANGED:   // INFO_OUTPUT_BUFFERS_CHANGED
            // not expected for an encoder; handle it anyway
            ALOGV("Encoder buffers changed");
            err = encoder->getOutputBuffers(&buffers);
            if (err != NO_ERROR) {
                fprintf(stderr,
                        "Unable to get new output buffers (err=%d)\n", err);
                return err;
            }
            break;
        case INVALID_OPERATION:
            fprintf(stderr, "Request for encoder buffer failed\n");
            return err;
        default:
            fprintf(stderr,
                    "Got weird result %d from dequeueOutputBuffer\n", err);
            return err;
        }
    }

    ALOGV("Encoder stopping (req=%d)", gStopRequested);
    if (gVerbose) {
        printf("Encoder stopping; recorded %u frames in %ld seconds\n",
                debugNumFrames, time(NULL) - debugStartWhen);
    }
    return NO_ERROR;
}

/*
 * Main "do work" method.
 *
 * Configures codec, muxer, and virtual display, then starts moving bits
 * around.
 */
static status_t recordScreen(const char* fileName) {
    status_t err;

    // Configure signal handler.
    err = configureSignals();
    if (err != NO_ERROR) return err;

    // Start Binder thread pool.  MediaCodec needs to be able to receive
    // messages from mediaserver.
    sp<ProcessState> self = ProcessState::self();
    self->startThreadPool();

    // Get main display parameters.
    sp<IBinder> mainDpy = SurfaceComposerClient::getBuiltInDisplay(
            ISurfaceComposer::eDisplayIdMain);
    DisplayInfo mainDpyInfo;
    err = SurfaceComposerClient::getDisplayInfo(mainDpy, &mainDpyInfo);
    if (err != NO_ERROR) {
        fprintf(stderr, "ERROR: unable to get display characteristics\n");
        return err;
    }
    if (gVerbose) {
        printf("Main display is %dx%d @%.2ffps (orientation=%u)\n",
                mainDpyInfo.w, mainDpyInfo.h, mainDpyInfo.fps,
                mainDpyInfo.orientation);
    }

    bool rotated = isDeviceRotated(mainDpyInfo.orientation);
    if (gVideoWidth == 0) {
        gVideoWidth = rotated ? mainDpyInfo.h : mainDpyInfo.w;
    }
    if (gVideoHeight == 0) {
        gVideoHeight = rotated ? mainDpyInfo.w : mainDpyInfo.h;
    }

    // Configure and start the encoder.
    sp<MediaCodec> encoder;
    sp<IGraphicBufferProducer> bufferProducer;
    err = prepareEncoder(mainDpyInfo.fps, &encoder, &bufferProducer);
    if (err != NO_ERROR && !gSizeSpecified) {
        ALOGV("Retrying with 720p");
        if (gVideoWidth != 1280 && gVideoHeight != 720) {
            fprintf(stderr, "WARNING: failed at %dx%d, retrying at 720p\n",
                    gVideoWidth, gVideoHeight);
            gVideoWidth = 1280;
            gVideoHeight = 720;
            err = prepareEncoder(mainDpyInfo.fps, &encoder, &bufferProducer);
        }
    }
    if (err != NO_ERROR) {
        return err;
    }

    // Configure virtual display.
    sp<IBinder> dpy;
    err = prepareVirtualDisplay(mainDpyInfo, bufferProducer, &dpy);
    if (err != NO_ERROR) return err;

    // Configure, but do not start, muxer.
    sp<MediaMuxer> muxer = new MediaMuxer(fileName,
            MediaMuxer::OUTPUT_FORMAT_MPEG_4);
    if (gRotate) {
        muxer->setOrientationHint(90);
    }

    // Main encoder loop.
    err = runEncoder(encoder, muxer);
    if (err != NO_ERROR) return err;

    if (gVerbose) {
        printf("Stopping encoder and muxer\n");
    }

    // Shut everything down, starting with the producer side.
    bufferProducer = NULL;
    SurfaceComposerClient::destroyDisplay(dpy);

    encoder->stop();
    muxer->stop();
    encoder->release();

    return 0;
}

/*
 * Sends a broadcast to the media scanner to tell it about the new video.
 */
static status_t notifyMediaScanner(const char* fileName) {
    String8 command("am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d file://");
    command.append(fileName);
    if (gVerbose) {
        printf("Shell: %s\n", command.string());
    }

    // TODO: for non-verbose mode we should suppress stdout
    int status = system(command.string());
    if (status < 0) {
        fprintf(stderr, "Unable to fork shell for media scanner broadcast\n");
        return UNKNOWN_ERROR;
    } else if (status != 0) {
        fprintf(stderr, "am command failed (status=%d): '%s'\n",
                status, command.string());
        return UNKNOWN_ERROR;
    }
    return NO_ERROR;
}

/*
 * Parses a string of the form "1280x720".
 *
 * Returns true on success.
 */
static bool parseWidthHeight(const char* widthHeight, uint32_t* pWidth,
        uint32_t* pHeight) {
    long width, height;
    char* end;

    // Must specify base 10, or "0x0" gets parsed differently.
    width = strtol(widthHeight, &end, 10);
    if (end == widthHeight || *end != 'x' || *(end+1) == '\0') {
        // invalid chars in width, or missing 'x', or missing height
        return false;
    }
    height = strtol(end + 1, &end, 10);
    if (*end != '\0') {
        // invalid chars in height
        return false;
    }

    *pWidth = width;
    *pHeight = height;
    return true;
}

/*
 * Dumps usage on stderr.
 */
static void usage() {
    fprintf(stderr,
        "Usage: screenrecord [options] <filename>\n"
        "\n"
        "Records the device's display to a .mp4 file.\n"
        "\n"
        "Options:\n"
        "--size WIDTHxHEIGHT\n"
        "    Set the video size, e.g. \"1280x720\".  For best results, use\n"
        "    a size supported by the AVC encoder.\n"
        "--bit-rate RATE\n"
        "    Set the video bit rate, in megabits per second.  Default 4Mbps.\n"
        "--rotate\n"
        "    Rotate the output 90 degrees.\n"
        "--verbose\n"
        "    Display interesting information on stdout.\n"
        "--help\n"
        "    Show this message.\n"
        "\n"
        "Recording continues until Ctrl-C is hit.\n"
        "\n"
        );
}

/*
 * Parses args and kicks things off.
 */
int main(int argc, char* const argv[]) {
    static const struct option longOptions[] = {
        { "help",       no_argument,        NULL, 'h' },
        { "verbose",    no_argument,        NULL, 'v' },
        { "size",       required_argument,  NULL, 's' },
        { "bit-rate",   required_argument,  NULL, 'b' },
        { "rotate",     no_argument,        NULL, 'r' },
        { NULL,         0,                  NULL, 0 }
    };

    while (true) {
        int optionIndex = 0;
        int ic = getopt_long(argc, argv, "", longOptions, &optionIndex);
        if (ic == -1) {
            break;
        }

        switch (ic) {
        case 'h':
            usage();
            return 0;
        case 'v':
            gVerbose = true;
            break;
        case 's':
            if (!parseWidthHeight(optarg, &gVideoWidth, &gVideoHeight)) {
                fprintf(stderr, "Invalid size '%s', must be width x height\n",
                        optarg);
                return 2;
            }
            if (gVideoWidth == 0 || gVideoHeight == 0) {
                fprintf(stderr,
                    "Invalid size %ux%u, width and height may not be zero\n",
                    gVideoWidth, gVideoHeight);
                return 2;
            }
            gSizeSpecified = true;
            break;
        case 'b':
            gBitRate = atoi(optarg);
            if (gBitRate < kMinBitRate || gBitRate > kMaxBitRate) {
                fprintf(stderr,
                        "Bit rate %dbps outside acceptable range [%d,%d]\n",
                        gBitRate, kMinBitRate, kMaxBitRate);
                return 2;
            }
            break;
        case 'r':
            gRotate = true;
            break;
        default:
            if (ic != '?') {
                fprintf(stderr, "getopt_long returned unexpected value 0x%x\n", ic);
            }
            return 2;
        }
    }

    if (optind != argc - 1) {
        fprintf(stderr, "Must specify output file (see --help).\n");
        return 2;
    }

    // MediaMuxer tries to create the file in the constructor, but we don't
    // learn about the failure until muxer.start(), which returns a generic
    // error code without logging anything.  We attempt to create the file
    // now for better diagnostics.
    const char* fileName = argv[optind];
    int fd = open(fileName, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        fprintf(stderr, "Unable to open '%s': %s\n", fileName, strerror(errno));
        return 1;
    }
    close(fd);

    status_t err = recordScreen(fileName);
    if (err == NO_ERROR) {
        // Try to notify the media scanner.  Not fatal if this fails.
        notifyMediaScanner(fileName);
    }
    ALOGD(err == NO_ERROR ? "success" : "failed");
    return (int) err;
}