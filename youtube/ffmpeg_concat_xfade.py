#!/usr/bin/python3

# https://gist.githubusercontent.com/royshil/369e175960718b5a03e40f279b131788/raw/0987e97022304bd2b42a794276f77136dbc7c62f/ffmpeg_concat_xfade.py

import argparse
import subprocess
import itertools


parser = argparse.ArgumentParser(description='Concatenate videos with FFMPEG, add "xfade" between segments.')
parser.add_argument('--segments_file', '-f', metavar='Segments file', type=str, nargs=1,
                    help='Segments text file for concatenating. e.g. "segments.txt"')
parser.add_argument('--output', '-o', dest='output_filename', type=str,
                    default='ffmpeg_concat_fade_out.mp4',
                    help='output filename to provide to ffmpeg. default="ffmpeg_concat_fade_out.mp4"')
parser.add_argument('segments', nargs='+')
args = parser.parse_args()

if args.segments_file:
    with open(args.segments_file[0], 'r') as seg_file:
        # cut the `file '` prefix and `'` postfix
        segments = [line[6:-2] for line in seg_file.readlines() if len(line.strip()) > 0 and line[0] != "#"]
else:
    segments = args.segments

# Get the lengths of the videos in seconds
file_lengths = [
    float(subprocess.run(['/usr/bin/ffprobe',
                          '-v', 'error',
                          '-show_entries', 'format=duration',
                          '-of', 'default=noprint_wrappers=1:nokey=1',
                          f],
                         capture_output=True).stdout.splitlines()[0])
    for f in segments
]

width = int(subprocess.run(['/usr/bin/ffprobe', '-v', 'error', '-select_streams', 'v', '-show_entries',
                            'stream=width', '-of', 'default=nw=1:nk=1', segments[0]],
                           capture_output=True).stdout.splitlines()[0])
height = int(subprocess.run(['/usr/bin/ffprobe', '-v', 'error', '-select_streams', 'v', '-show_entries',
                             'stream=height', '-of', 'default=nw=1:nk=1', segments[0]],
                            capture_output=True).stdout.splitlines()[0])

# File inputs from the list
files_input = [['-i', f] for f in segments]

# Prepare the filter graph
video_fades = ""
audio_fades = ""
last_fade_output = "0v"
last_audio_output = "0:a"
video_length = 0
normalizer = ""
scaler_default = f",scale=w={width}:h={height}:force_original_aspect_ratio=1,pad={width}:{height}:(ow-iw)/2:(oh-ih)/2"

for i in range(len(segments)):
    # Videos normalizer
    scaler = scaler_default if i > 0 else ""
    normalizer += f"[{i}:v]settb=AVTB,setsar=sar=1,fps=30{scaler}[{i}v];"

    if i == 0:
        continue

    # Video graph: chain the xfade operator together
    video_length += file_lengths[i - 1] - 0.25
    next_fade_output = "v%d%d" % (i - 1, i)
    video_fades += "[%s][%dv]xfade=duration=0.5:offset=%.3f[%s];" % \
        (last_fade_output, i, video_length - 1, next_fade_output)
    last_fade_output = next_fade_output

    # Audio graph:
    next_audio_output = "a%d%d" % (i - 1, i)
    audio_fades += f"[{last_audio_output}][{i}:a]acrossfade=d=1[{next_audio_output}];"
    last_audio_output = next_audio_output

video_fades += f"[{last_fade_output}]format=pix_fmts=yuv420p[final];"

# Assemble the FFMPEG command arguments
ffmpeg_args = ['/usr/bin/ffmpeg',
               *itertools.chain(*files_input),
               '-filter_complex', normalizer + video_fades + audio_fades[:-1],
               '-map', '[final]',
               '-map', f"[{last_audio_output}]",
               '-y',
               args.output_filename]

print(" ".join(ffmpeg_args))
# Run FFMPEG
subprocess.run(ffmpeg_args)
