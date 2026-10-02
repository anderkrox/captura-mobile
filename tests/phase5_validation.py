"""Acceptance checks for real-environment recordings and resource observations."""

from __future__ import annotations

import math
import statistics


def memory_trend(samples, *, warmup_seconds=120):
    """Use private bytes after warmup, rather than the working-set high-water mark."""
    points = [(s['elapsed_seconds'], s['private_bytes'] / 1048576)
              for s in samples if s['elapsed_seconds'] >= warmup_seconds]
    if len(points) < 4 or points[-1][0] - points[0][0] < 60:
        raise ValueError('Insufficient post-warmup memory samples.')
    if any(not math.isfinite(x) or not math.isfinite(y) or y < 0 for x, y in points):
        raise ValueError('Invalid memory sample.')
    if any(b[0] <= a[0] for a, b in zip(points, points[1:])):
        raise ValueError('Sample timestamps must increase.')
    xs, ys = zip(*points)
    average_x, average_y = statistics.mean(xs), statistics.mean(ys)
    slope = sum((x - average_x) * (y - average_y) for x, y in points) / sum(
        (x - average_x) ** 2 for x in xs) * 60
    window = max(1, len(ys) // 4)
    growth = statistics.median(ys[-window:]) - statistics.median(ys[:window])
    return {'samples': len(points), 'slope_mib_per_minute': slope,
            'median_growth_mib': growth, 'peak_private_mib': max(ys),
            'passed': slope <= 1.0 and growth <= 32.0}


def check_media(metadata, frames, *, minimum_seconds=0):
    streams = metadata.get('streams', [])
    if len(streams) != 1:
        raise ValueError('Expected one video stream and no audio.')
    expected = {'codec_type': 'video', 'codec_name': 'h264', 'width': 1080, 'height': 1920,
                'pix_fmt': 'yuv420p', 'r_frame_rate': '30/1', 'avg_frame_rate': '30/1',
                'sample_aspect_ratio': '1:1', 'display_aspect_ratio': '9:16',
                'color_space': 'bt709', 'color_transfer': 'bt709', 'color_primaries': 'bt709'}
    for key, value in expected.items():
        if streams[0].get(key) != value:
            raise ValueError(f'{key}: {streams[0].get(key)!r}, expected {value!r}.')
    if frames <= 0 or int(streams[0].get('nb_read_frames', 0)) != frames:
        raise ValueError('Decoded frame count differs from the recording.')
    duration = float(metadata['format']['duration'])
    if not math.isfinite(duration) or duration < minimum_seconds or abs(duration - frames / 30) > 1 / 30:
        raise ValueError('Duration differs from the expected 30 FPS timeline.')
    return {'frames': frames, 'duration_seconds': duration, 'resolution': '1080x1920',
            'fps': 30, 'audio_streams': 0}


def packet_progress(samples):
    """Check advancement, allowing Matroska clusters and file I/O to buffer packets.

    The age of the last readable packet is not the encoder's processing latency.
    https://ffmpeg.org/ffmpeg-formats.html#matroska
    """
    if len(samples) < 3:
        raise ValueError('Insufficient muxed-packet progress samples.')
    elapsed = [float(sample['elapsed_seconds']) for sample in samples]
    muxed = [float(sample['muxed_seconds']) for sample in samples]
    if any(not math.isfinite(value) for value in elapsed + muxed):
        raise ValueError('Nonfinite packet progress sample.')
    if any(b <= a for a, b in zip(elapsed, elapsed[1:])) or any(b <= a for a, b in zip(muxed, muxed[1:])):
        raise ValueError('Wall clock and muxed packets must advance.')
    lags = [wall - pts for wall, pts in zip(elapsed, muxed)]
    elapsed_mean, lag_mean = statistics.mean(elapsed), statistics.mean(lags)
    slope = sum((wall - elapsed_mean) * (lag - lag_mean) for wall, lag in zip(elapsed, lags)) / sum(
        (wall - elapsed_mean) ** 2 for wall in elapsed) * 60
    speed = (muxed[-1] - muxed[0]) / (elapsed[-1] - elapsed[0])
    return {'muxed_progress_ratio': speed, 'buffered_packet_age_max_seconds': max(lags),
            'packet_age_slope_seconds_per_minute': slope, 'passed': speed >= .99 and slope <= .1,
            'scope': 'Readable muxed-packet advancement; does not measure encoder processing latency.'}


def corner_errors(reference, exported, width=486, height=864, patch=32):
    expected = width * height * 3
    if len(reference) != expected or len(exported) != expected:
        raise ValueError('RGB frame has the wrong size.')
    if patch <= 0 or patch > min(width, height) // 2:
        raise ValueError('Invalid corner patch size.')
    errors = []
    for left, top in ((0, 0), (width - patch, 0), (0, height - patch),
                      (width - patch, height - patch)):
        total = 0
        for y in range(top, top + patch):
            start = (y * width + left) * 3
            total += sum(abs(a - b) for a, b in zip(reference[start:start + patch * 3],
                                                   exported[start:start + patch * 3]))
        errors.append(total / (patch * patch * 3))
    return errors
