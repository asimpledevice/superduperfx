# SuperDuperFX: no H.264 (h264_codec, and lspr3 sprites in the main
# Makefile). Its two RSP overlays fill IMEM next to stock rspq (8 bytes
# spare) and cannot fit beside Red's resident kernel; nothing in SuperDuperFX
# uses them.
LIBDRAGON_OBJS += \
	$(BUILD_DIR)/video/video.o \
	$(BUILD_DIR)/video/video_sync.o \
	$(BUILD_DIR)/video/fmv.o \
	$(BUILD_DIR)/video/mpeg1.o \
	$(BUILD_DIR)/video/yuv.o \
	$(BUILD_DIR)/video/rsp_yuv.o \
	$(BUILD_DIR)/video/rsp_mpeg1.o \
	$(BUILD_DIR)/video/subtitles.o
