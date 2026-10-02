#!/usr/bin/env python3
#
#  trim_pl_mpeg.py --- third_party/pl_mpeg.h as r2 can run it.
#
#  Usage: tools/trim_pl_mpeg.py third_party/pl_mpeg.h pl_mpeg_r2.h
#
#  r2 does not keep a task's SSE or x87 registers across a context switch, so
#  the player is built with -mgeneral-regs-only: not one floating-point
#  instruction may be in it.  pl_mpeg's video decoding is integer arithmetic
#  already; what is not is the bookkeeping around it and the audio decoder.
#  So this keeps the buffer, the demuxer and the video decoder, and:
#
#    - drops the high-level plm_* player and the MP2 audio decoder whole;
#    - keeps time in integers: a packet's PTS in 90 kHz ticks, a frame's time
#      and the decoder's clock in milliseconds;
#    - gives the frame rate as frames per 1000 s (25 fps is 25000, 29.97 is
#      29970), and drops the pixel aspect ratio;
#    - drops plm_demux_seek(), which is built on seconds throughout.
#
#  Every edit is checked to have found what it expected, so a newer upstream
#  that moved things fails here rather than building into something else.
#

import re
import sys

src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()

SEP = "// -----------------------------------------------------------------------------\n"


def sub(old, new, count=1):
    global s
    n = s.count(old)
    if n != count:
        sys.exit(f"trim_pl_mpeg: expected {count} of {old[:70]!r}, found {n}")
    s = s.replace(old, new)


def cut(start, end, keep_end=True):
    """Removes from `start` up to `end` (which stays, unless keep_end is off)."""
    global s
    a = s.find(start)
    if a < 0:
        sys.exit(f"trim_pl_mpeg: no {start[:70]!r}")
    b = s.find(end, a + len(start))
    if b < 0:
        sys.exit(f"trim_pl_mpeg: no {end[:70]!r} after {start[:40]!r}")
    s = s[:a] + s[b if keep_end else b + len(end):]


# ── Public types ─────────────────────────────────────────────────────────────

sub("typedef struct plm_t plm_t;\n", "")
sub("typedef struct plm_audio_t plm_audio_t;\n", "")
sub("presentation time stamp of the packet in seconds.",
    "presentation time stamp of the packet in 90 kHz ticks (r2: an integer).")
sub("\tint type;\n\tdouble pts;\n", "\tint type;\n\tint64_t pts;\n")
sub("typedef struct {\n\tdouble time;\n\tunsigned int width;",
    "typedef struct {\n\tint64_t time; // milliseconds (r2: an integer)\n\tunsigned int width;")
# The callbacks of the high-level player, and the audio samples.
cut("// Callback function type for decoded video frames used by the high-level",
    "// Callback function for plm_buffer when it needs more data")

# ── Public API ───────────────────────────────────────────────────────────────

cut(SEP + "// plm_* public API", SEP + "// plm_buffer public API")
#  The audio API first: its comments repeat the video decoder's word for word.
cut(SEP + "// plm_audio public API", "#ifdef __cplusplus\n}\n#endif\n\n#endif // PL_MPEG_H")
cut("// Seek to a packet of the specified type with a PTS just before specified time.",
    "// Get the PTS of the first packet of this type.")
sub("double plm_demux_get_start_time(plm_demux_t *self, int type);",
    "int64_t plm_demux_get_start_time(plm_demux_t *self, int type);")
sub("double plm_demux_get_duration(plm_demux_t *self, int type);",
    "int64_t plm_demux_get_duration(plm_demux_t *self, int type);")
sub("// Get the framerate in frames per second.\n\ndouble plm_video_get_framerate(plm_video_t *self);\n"
    "double plm_video_get_pixel_aspect_ratio(plm_video_t *self);",
    "// Get the framerate in frames per 1000 seconds (25 fps is 25000).\n\n"
    "int plm_video_get_framerate_milli(plm_video_t *self);")
sub("// Get the current internal time in seconds.\n\ndouble plm_video_get_time(plm_video_t *self);",
    "// Get the current internal time in milliseconds.\n\nint64_t plm_video_get_time(plm_video_t *self);")
sub("// Set the current internal time in seconds. This is only useful when you\n// manipulate the underlying video buffer",
    "// Set the current internal time in milliseconds. This is only useful when you\n// manipulate the underlying video buffer")
sub("void plm_video_set_time(plm_video_t *self, double time);",
    "void plm_video_set_time(plm_video_t *self, int64_t time);")

# ── Implementation ───────────────────────────────────────────────────────────

cut(SEP + "// plm (high-level interface) implementation", SEP + "// plm_buffer implementation")
cut(SEP + "// plm_audio implementation", "#endif // PL_MPEG_IMPLEMENTATION")

# The demuxer: its clock in 90 kHz ticks.
sub("\tdouble system_clock_ref;\n", "\tint64_t system_clock_ref;\n")
sub("\tdouble last_decoded_pts;\n\tdouble start_time;\n\tdouble duration;\n",
    "\tint64_t last_decoded_pts;\n\tint64_t start_time;\n\tint64_t duration;\n")
sub("double plm_demux_decode_time(plm_demux_t *self);", "int64_t plm_demux_decode_time(plm_demux_t *self);")
sub("double plm_demux_decode_time(plm_demux_t *self) {", "int64_t plm_demux_decode_time(plm_demux_t *self) {")
sub("\treturn (double)clock / 90000.0;\n", "\treturn clock;\n")
sub("double plm_demux_get_start_time(plm_demux_t *self, int type) {",
    "int64_t plm_demux_get_start_time(plm_demux_t *self, int type) {")
sub("double plm_demux_get_duration(plm_demux_t *self, int type) {",
    "int64_t plm_demux_get_duration(plm_demux_t *self, int type) {")
sub("\t\tdouble last_pts = PLM_PACKET_INVALID_TS;\n", "\t\tint64_t last_pts = PLM_PACKET_INVALID_TS;\n")
cut("plm_packet_t *plm_demux_seek(plm_demux_t *self, double seek_time, int type, int force_intra) {",
    "plm_packet_t *plm_demux_decode(plm_demux_t *self) {")

# The video decoder: rate as frames per 1000 s, time in milliseconds.
cut("static const float PLM_VIDEO_PIXEL_ASPECT_RATIO[] = {", "static const double PLM_VIDEO_PICTURE_RATE[] = {")
sub("static const double PLM_VIDEO_PICTURE_RATE[] = {\n"
    "\t0.000, 23.976, 24.000, 25.000, 29.970, 30.000, 50.000, 59.940,\n"
    "\t60.000, 0.000, 0.000, 0.000, 0.000, 0.000, 0.000, 0.000\n};",
    "static const int PLM_VIDEO_PICTURE_RATE[] = {\n"
    "\t0, 23976, 24000, 25000, 29970, 30000, 50000, 59940,\n"
    "\t60000, 0, 0, 0, 0, 0, 0, 0\n};")
sub("\tdouble framerate;\n\tdouble pixel_aspect_ratio;\n\tdouble time;\n",
    "\tint framerate; // frames per 1000 s\n\tint64_t time; // milliseconds\n")
sub("double plm_video_get_framerate(plm_video_t *self) {",
    "int plm_video_get_framerate_milli(plm_video_t *self) {")
cut("double plm_video_get_pixel_aspect_ratio(plm_video_t *self) {", "int plm_video_get_width(plm_video_t *self) {")
sub("double plm_video_get_time(plm_video_t *self) {", "int64_t plm_video_get_time(plm_video_t *self) {")
sub("void plm_video_set_time(plm_video_t *self, double time) {\n"
    "\tself->frames_decoded = self->framerate * time;\n",
    "void plm_video_set_time(plm_video_t *self, int64_t time) {\n"
    "\tself->frames_decoded = (int)(time * self->framerate / 1000000);\n")
sub("\tself->time = (double)self->frames_decoded / self->framerate;\n",
    "\tself->time = self->framerate\n"
    "\t\t? (int64_t)self->frames_decoded * 1000000 / self->framerate\n"
    "\t\t: 0;\n")
# The aspect ratio is still in the header and has to be read past.
cut("\tint pixel_aspect_ratio_code;\n", "\t// Get frame rate\n")
sub("\t// Get pixel aspect ratio\n\t// Get frame rate\n",
    "\t// Skip the pixel aspect ratio (r2: not kept)\n\tplm_buffer_skip(self->buffer, 4);\n\n\t// Get frame rate\n")

#  Nothing floating-point may be left in the code (comments may say "double").
code = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), s, flags=re.S)
for n, line in enumerate(code.splitlines(), 1):
    if re.search(r"\b(double|float)\b", line.split("//")[0]):
        sys.exit(f"trim_pl_mpeg: floating point left on line {n}: {line.strip()}")

banner = ("// pl_mpeg_r2.h --- generated by tools/trim_pl_mpeg.py from third_party/pl_mpeg.h;\n"
          "// edit the script, not this file.  Video only, and no floating point: see\n"
          "// the script for what was changed and why.\n\n")
open(dst, "w").write(banner + s)
