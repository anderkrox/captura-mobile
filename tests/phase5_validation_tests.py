"""Unit tests for the acceptance checks, including misleading pass conditions."""

from pathlib import Path
import sys
import unittest
import xml.etree.ElementTree as ET

from phase5_validation import check_media, corner_errors, memory_trend, packet_progress


def metadata():
    return {'streams': [{'codec_type': 'video', 'codec_name': 'h264', 'width': 1080, 'height': 1920,
        'pix_fmt': 'yuv420p', 'r_frame_rate': '30/1', 'avg_frame_rate': '30/1',
        'sample_aspect_ratio': '1:1', 'display_aspect_ratio': '9:16', 'color_space': 'bt709',
        'color_transfer': 'bt709', 'color_primaries': 'bt709', 'nb_read_frames': '54000'}],
        'format': {'duration': '1800.000000'}}


def samples(rate=0, offset=0):
    return [{'elapsed_seconds': t, 'private_bytes': (100 + offset + rate * t / 60) * 1048576}
            for t in range(0, 1801, 30)]


class MemoryTests(unittest.TestCase):
    def test_stable_private_memory(self):
        self.assertTrue(memory_trend(samples())['passed'])

    def test_small_bounded_allocator_growth(self):
        self.assertTrue(memory_trend(samples(.1))['passed'])

    def test_progressive_leak_fails(self):
        self.assertFalse(memory_trend(samples(2))['passed'])

    def test_large_slow_growth_fails_median_limit(self):
        points = [{'elapsed_seconds': t, 'private_bytes': (100 + .9 * t / 60) * 1048576}
                  for t in range(0, 7201, 30)]
        self.assertFalse(memory_trend(points)['passed'])

    def test_initialization_allocation_is_excluded(self):
        points = samples()
        points[0]['private_bytes'] = 1048576
        self.assertAlmostEqual(memory_trend(points)['slope_mib_per_minute'], 0)

    def test_constant_high_water_mark_is_not_a_leak(self):
        self.assertTrue(memory_trend(samples(offset=500))['passed'])

    def test_insufficient_samples(self):
        with self.assertRaises(ValueError): memory_trend(samples()[:3])

    def test_duplicate_timestamps(self):
        points = samples()
        points[-1]['elapsed_seconds'] = points[-2]['elapsed_seconds']
        with self.assertRaises(ValueError): memory_trend(points)

    def test_nonfinite_private_memory(self):
        points = samples()
        points[-1]['private_bytes'] = float('nan')
        with self.assertRaises(ValueError): memory_trend(points)

    def test_negative_private_memory(self):
        points = samples()
        points[-1]['private_bytes'] = -1
        with self.assertRaises(ValueError): memory_trend(points)


class MediaTests(unittest.TestCase):
    def test_real_30_minute_timeline(self):
        self.assertEqual(check_media(metadata(), 54000, minimum_seconds=1800)['duration_seconds'], 1800)

    def test_silent_audio_stream_is_rejected(self):
        data = metadata()
        data['streams'].append({'codec_type': 'audio'})
        with self.assertRaises(ValueError): check_media(data, 54000)

    def test_empty_recording_is_rejected(self):
        with self.assertRaises(ValueError): check_media(metadata(), 0)

    def test_missing_video_stream_is_rejected(self):
        data = metadata()
        data['streams'] = []
        with self.assertRaises(ValueError): check_media(data, 54000)

    def test_truncated_decoded_frame_count_is_rejected(self):
        with self.assertRaises(ValueError): check_media(metadata(), 53999)

    def test_accelerated_time_is_rejected(self):
        data = metadata()
        data['format']['duration'] = '60'
        with self.assertRaises(ValueError): check_media(data, 54000, minimum_seconds=1800)

    def test_nan_duration_is_rejected(self):
        data = metadata()
        data['format']['duration'] = 'NaN'
        with self.assertRaises(ValueError): check_media(data, 54000)

    def test_short_endurance_is_rejected(self):
        data = metadata()
        data['format']['duration'] = '1799'
        data['streams'][0]['nb_read_frames'] = '53970'
        with self.assertRaises(ValueError): check_media(data, 53970, minimum_seconds=1800)


for field, incorrect in {'width': 1920, 'height': 1080, 'codec_name': 'hevc', 'pix_fmt': 'yuv444p',
                        'avg_frame_rate': '60/1', 'r_frame_rate': '60/1', 'sample_aspect_ratio': '2:1',
                        'display_aspect_ratio': '1:1', 'color_space': 'bt470bg'}.items():
    def incorrect_media(self, field=field, incorrect=incorrect):
        data = metadata()
        data['streams'][0][field] = incorrect
        with self.assertRaises(ValueError): check_media(data, 54000)
    setattr(MediaTests, f'test_rejects_incorrect_{field}', incorrect_media)


class CornerTests(unittest.TestCase):
    def test_identical_corners(self):
        self.assertEqual(corner_errors(bytes(48), bytes(48), 4, 4, 2), [0, 0, 0, 0])

    def test_lost_bottom_edge_is_observed(self):
        frame = bytes(4 * 3 * 3) + bytes([255] * 4 * 3)
        errors = corner_errors(bytes(48), frame, 4, 4, 2)
        self.assertEqual(errors, [0, 0, 127.5, 127.5])

    def test_center_animation_does_not_change_corners(self):
        frame = bytearray(108)
        frame[(3 * 6 + 3) * 3:(3 * 6 + 3) * 3 + 3] = bytes([255] * 3)
        self.assertEqual(corner_errors(bytes(108), frame, 6, 6, 2), [0, 0, 0, 0])

    def test_truncated_frame_is_rejected(self):
        with self.assertRaises(ValueError): corner_errors(bytes(47), bytes(48), 4, 4, 2)

    def test_overlapping_patches_are_rejected(self):
        with self.assertRaises(ValueError): corner_errors(bytes(48), bytes(48), 4, 4, 3)


class PacketProgressTests(unittest.TestCase):
    def test_buffered_clusters_do_not_imply_encoder_delay(self):
        points = [{'elapsed_seconds': t, 'muxed_seconds': t - 8} for t in (300, 600, 900)]
        self.assertTrue(packet_progress(points)['passed'])

    def test_increasing_backlog_fails(self):
        points = [{'elapsed_seconds': t, 'muxed_seconds': t * .95} for t in (300, 600, 900)]
        self.assertFalse(packet_progress(points)['passed'])

    def test_stalled_packets_fail(self):
        points = [{'elapsed_seconds': t, 'muxed_seconds': 290} for t in (300, 600, 900)]
        with self.assertRaises(ValueError): packet_progress(points)

    def test_insufficient_samples_fail(self):
        with self.assertRaises(ValueError): packet_progress([])

    def test_nonfinite_packets_fail(self):
        points = [{'elapsed_seconds': t, 'muxed_seconds': float('nan')} for t in (300, 600, 900)]
        with self.assertRaises(ValueError): packet_progress(points)


class JUnitResult(unittest.TextTestResult):
    def startTest(self, test):
        super().startTest(test)
        self.nodes = getattr(self, 'nodes', [])
        self.node = ET.Element('testcase', name=test.id())
        self.nodes.append(self.node)

    def addFailure(self, test, error):
        super().addFailure(test, error)
        ET.SubElement(self.node, 'failure').text = self._exc_info_to_string(error, test)

    def addError(self, test, error):
        super().addError(test, error)
        ET.SubElement(self.node, 'error').text = self._exc_info_to_string(error, test)


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2, resultclass=JUnitResult).run(suite)
    if len(sys.argv) > 1:
        target = Path(sys.argv[1])
        target.parent.mkdir(parents=True, exist_ok=True)
        root = ET.Element('testsuite', name='phase5_validation', tests=str(result.testsRun),
                          failures=str(len(result.failures)), errors=str(len(result.errors)))
        root.extend(result.nodes)
        ET.ElementTree(root).write(target, encoding='utf-8', xml_declaration=True)
    raise SystemExit(0 if result.wasSuccessful() else 1)
