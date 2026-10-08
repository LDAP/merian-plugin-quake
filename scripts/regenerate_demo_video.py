import json
import os
import shutil
import subprocess
import sys

import imageio_ffmpeg
from PIL import Image

CLIPS = [
    ('alk_start', 'alk', 'start', (832, 60, 536, 0, 180), (832, -65, 536, 0, 180), 150, 0, []),
    ('ad_tears', 'ad', 'ad_tears', (760, -256, -470, 12, 180), (-55, -256, -642, 12, 180), 188, 60, []),
    ('ad_start', 'ad', 'start', (-352, 700, 410, -8, 90), (-352, 910, 413, -8, 90), 150, None, []),
    ('immortal_start', 'immortal', 'start', (-280, -140, 86, -8, 245), (-700, -140, 86, -8, 245), 150, None, []),
    ('qbj3_hall', 'qbj3', 'start', (-1950, 130, 2000, -10, 90), (-2450, 130, 2000, -10, 90), 210, None, []),
    ('ad_sepulcher', 'ad', 'ad_sepulcher', (-2540, 2400, 400, -5, 330), (-2540, 2400, 400, -5, 210), 240, 60, []),
    ('ad_azad', 'ad', 'ad_azad', (300, -3200, 2300, 8, 90), (300, -1700, 2300, 8, 90), 150, None, ['--volume', 'on']),
    ('ad_grendel', 'ad', 'ad_grendel', (-170, 0, 150, 20, 0), (-170, 0, 870, 55, 0), 240, 100, ['--volume', 'on']),
]
FPS = 30
HOLD = 210
SIZE = (1280, 720)
FADE = 8
QUALITY = 60

plugin = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
quake_dir = os.path.abspath(sys.argv[1])
build = os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else os.path.join(plugin, '..', '..', 'build'))
only = sys.argv[3:]
out = os.path.join(build, 'demo-video')
images = os.path.join(plugin, 'images')


def camera_script(start, end, frames):
    def origin(t):
        return ' '.join(f'{start[k] + (end[k] - start[k]) * t:.2f}' for k in range(3))

    seconds = frames / FPS
    look = []
    for index, positive, negative, cvar in ((3, '+lookdown', '+lookup', 'cl_pitchspeed'),
                                            (4, '+left', '+right', 'cl_yawspeed')):
        speed = (end[index] - start[index]) / seconds
        if speed:
            look.append((f'{cvar} {abs(speed):.4f}', positive if speed > 0 else negative))
    lines = ['wait'] * 90 + ['god', 'notarget', 'lookspring 0', 'cl_nolerp 1',
                             f'setpos {origin(0)} {start[3]} {start[4]} 0', 'wait']
    for step in range(HOLD):
        lines += {0: ['+jump'], 1: ['-jump']}.get(step % 20, [])
        lines += [f'setpos {origin(0)}', 'wait']
    lines += [cvar for cvar, _ in look] + [key for _, key in look]
    for i in range(frames):
        lines += [f'setpos {origin(i / (frames - 1))}', 'wait']
    return '\n'.join(lines + ['-' + key[1:] for _, key in look])


def render(name, game, bsp, start, end, frames, args):
    directory = os.path.join(out, name)
    shutil.rmtree(directory, ignore_errors=True)
    os.makedirs(directory)
    cfg = os.path.join(quake_dir, game, 'demo_video.cfg')
    with open(cfg, 'w') as f:
        f.write(camera_script(start, end, frames))
    capture = {'nodes': {
        'output': {'enabled': False}, 'blit': {'enabled': False}, 'imgui': {'enabled': False},
        'gbuffer': {'properties': {'5': False, '6': False}},
        'render': {'properties': {'instance_mask': {'5': False, '6': False}}},
        'tonemap': {'$+$outputs': ['out->write.src']},
        'write': {'enabled': True, 'type': 'Image Write', 'properties': {
            'format': 'PNG', 'filename': os.path.join(directory, 'f_{image_index_total:04}').replace('\\', '/'),
            'enable': True, 'trigger': 'iteration', 'iteration': 96 + HOLD}}}}
    capture_file = os.path.join(directory, 'capture.json')
    with open(capture_file, 'w') as f:
        json.dump(capture, f)
    command = ['meson', 'devenv', '-C', build, 'merian-graph-run', os.path.join(plugin, 'quake.json'),
               '--denoiser', 'dlss', *args, '--quality', 'ultra_quality', '--merge', capture_file, f'--max-iterations={95 + HOLD + frames}',
               f'--time-delta={1000 / FPS:.4f}', '-nosound', '-basedir', quake_dir, '-game', game,
               '+map', bsp, '+exec', 'demo_video.cfg']
    with open(os.path.join(directory, 'run.log'), 'w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    os.remove(cfg)
    files = sorted(f for f in os.listdir(directory) if f.endswith('.png'))
    print(f'{name}: {len(files)} frames')
    return [os.path.join(directory, f) for f in files[:frames]]


segments = []
for name, game, bsp, start, end, frames, screenshot, args in CLIPS:
    directory = os.path.join(out, name)
    if not only or name in only or not os.path.isdir(directory):
        files = render(name, game, bsp, start, end, frames, args)
    else:
        files = sorted(os.path.join(directory, f) for f in os.listdir(directory) if f.endswith('.png'))[:frames]
    segment = [Image.open(f).convert('RGB').resize(SIZE, Image.LANCZOS) for f in files]
    if screenshot is not None:
        segment[screenshot].save(os.path.join(images, f'{name}.jpg'), quality=85, optimize=True)
    segments.append(segment)

video = list(segments[0])
for segment in segments[1:]:
    previous, video = video[-FADE:], video[:-FADE]
    video += [Image.blend(previous[k], segment[k], (k + 1) / (FADE + 1)) for k in range(FADE)] + segment[FADE:]
n = len(video)
video = [Image.blend(video[n - FADE + k], video[k], (k + 1) / (FADE + 1)) for k in range(FADE)] + video[FADE:n - FADE]
target = os.path.join(images, 'demo.avif')
video[0].save(target, save_all=True, append_images=video[1:], duration=round(1000 / FPS), loop=0, quality=QUALITY,
              speed=6)
mp4 = os.path.join(images, 'demo.mp4')
encoder = subprocess.Popen([imageio_ffmpeg.get_ffmpeg_exe(), '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                            '-s', f'{SIZE[0]}x{SIZE[1]}', '-r', str(FPS), '-i', '-', '-c:v', 'libx264', '-crf', '20',
                            '-preset', 'slow', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', mp4], stdin=subprocess.PIPE)
for frame in video:
    encoder.stdin.write(frame.tobytes())
encoder.stdin.close()
encoder.wait()
print(f'{len(video) / FPS:.1f} s, {os.path.getsize(target) / 1e6:.1f} MB avif, {os.path.getsize(mp4) / 1e6:.1f} MB mp4')
