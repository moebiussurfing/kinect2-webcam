"""Preset storage and GTK panel regression tests. Never touches the real service/config."""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

os.environ['KINECT_WEBCAM_APP_ID'] = 'local.manu.KinectWebcam.Tests'
source = os.environ.get('KINECT_GUI_SOURCE', str(Path(__file__).with_name('kinect-webcam-app')))
loader = importlib.machinery.SourceFileLoader('kinect_gui', source)
spec = importlib.util.spec_from_loader(loader.name, loader)
m = importlib.util.module_from_spec(spec)
loader.exec_module(m)


class StorageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / 'presets.json'
        self.patcher = patch.object(m, 'PRESETS_PATH', self.path)
        self.patcher.start()
        self.addCleanup(self.patcher.stop)
        self.cfg = {**m.DEFAULTS, **m.STRING_DEFAULTS}

    def test_round_trip_multiple_unicode_names(self):
        self.assertEqual(m.read_presets(), {})
        m.save_preset(' Día / cálido ', self.cfg)
        changed = {**self.cfg, 'look': 'noir', 'gamma': 1.7, 'lut_preset': 'golden_hour'}
        m.save_preset('Noche', changed)
        self.assertEqual(m.read_presets(), {'Día / cálido': self.cfg, 'Noche': changed})
        self.assertEqual(sorted(p.name for p in self.path.parent.iterdir()), ['presets.json'])

    def test_duplicate_and_bad_values_preserve_original(self):
        m.save_preset('Original', self.cfg)
        before = self.path.read_bytes()
        with self.assertRaises(ValueError):
            m.save_preset('Original', {**self.cfg, 'gamma': 2})
        for key, value in [('gamma', float('nan')), ('gain', 9), ('look', 'noir\ngain = 4'),
                           ('mirror', .5), ('exposure', 'oops')]:
            with self.subTest(key=key), self.assertRaises(ValueError):
                m.save_preset('Invalid', {**self.cfg, key: value})
        for name in (' ', 'x' * 81, 'line\nbreak'):
            with self.assertRaises(ValueError):
                m.save_preset(name, self.cfg)
        self.assertEqual(self.path.read_bytes(), before)

    def test_corrupt_file_is_not_overwritten(self):
        for raw in ('broken', '[]', '{"version": 2, "presets": {}}',
                    '{"version": 1, "presets": {"Incomplete": {}}}'):
            self.path.write_text(raw)
            with self.assertRaises(ValueError):
                m.save_preset('New', self.cfg)
            self.assertEqual(self.path.read_text(), raw)

    def test_twenty_distinct_valid_factory_presets(self):
        import json
        self.assertEqual(len(m.FACTORY_RECIPES), 20)
        self.assertEqual(len(m.FACTORY_PRESETS), 20)
        self.assertEqual(len({json.dumps(cfg, sort_keys=True) for cfg in m.FACTORY_PRESETS.values()}), 20)
        for name, cfg in m.FACTORY_PRESETS.items():
            with self.subTest(preset=name):
                self.assertEqual(m.validate_preset(cfg), cfg)
                self.assertIn(cfg['look'], dict(m.FALLBACK_LOOKS))
                self.assertIn(cfg['lut_preset'], ['none'] + list(dict(m.FALLBACK_LUTS)))
                self.assertEqual(cfg['lut'], '')
        self.assertFalse(self.path.exists())


class PanelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        m.Gtk.init_check()
        if m.Gdk.Display.get_default() is None:
            raise unittest.SkipTest('A graphical session is required for panel tests')
        m.Adw.init()
        cls.app = m.App()
        cls.app.register(None)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        for key, value in {
            'PRESETS_PATH': Path(self.tmp.name) / 'presets.json',
            'CONFIG_PATH': Path(self.tmp.name) / 'camera.conf',
            'service_active': lambda: False, 'service_enabled': lambda: False,
            'kinect_connected': lambda: True,
            'list_looks': lambda: m.FALLBACK_LOOKS,
            'list_luts': lambda: m.FALLBACK_LUTS,
        }.items():
            p = patch.object(m, key, value)
            p.start()
            self.addCleanup(p.stop)
        p = patch.object(m, 'systemctl')
        self.ctl = p.start()
        self.addCleanup(p.stop)
        p = patch.object(m, 'Preview')
        p.start()
        self.addCleanup(p.stop)
        self.win = m.Window(self.app)
        self.addCleanup(self.cleanup_window)

    def cleanup_window(self):
        self.win.on_close()
        self.win.destroy()

    def test_initial_groups_and_no_config_write(self):
        self.assertEqual([e.get_expanded() for e in self.win.group_expanders], [True, True, False, False, False])
        self.assertFalse(m.CONFIG_PATH.exists())
        self.ctl.assert_not_called()
        for expander in self.win.group_expanders:
            expander.set_expanded(not expander.get_expanded())
        self.assertEqual([e.get_expanded() for e in self.win.group_expanders], [False, False, True, True, True])

    def test_open_starts_camera_but_does_not_enable_login(self):
        with patch.object(self.win, '_run_async') as run:
            self.app.do_activate()
            run.assert_called_once_with('start', m.SERVICE)
        with patch.object(self.win, '_run_async') as run, patch.object(m, 'service_active', return_value=True):
            self.app.do_activate()
            run.assert_not_called()
        self.assertFalse(self.win.login_row.get_active())

    def test_save_dialog_and_load_full_settings_once(self):
        w = self.win
        w.gamma_row.set_value(1.8)
        w.look_row.set_selected(3)
        w.manual_row.set_active(True)
        w.ms_row.set_value(35)
        w.on_save_preset(None)
        dialog = w.get_visible_dialog()
        self.assertIsNotNone(dialog)
        dialog.get_extra_child().set_text('Studio cálido')
        dialog.emit('response', 'save')
        dialog.close()
        saved = m.read_presets()['Studio cálido']
        self.assertEqual(saved['gamma'], 1.8)
        self.assertEqual(saved['look'], 'bw')
        self.assertEqual(saved['exposure_ms'], 35)
        w.gamma_row.set_value(.5)
        w.look_row.set_selected(0)
        w.preset_row.set_selected(0)
        w.preset_row.set_selected(w._preset_entries.index(('user', 'Studio cálido')))
        self.assertEqual(w._collect(), saved)
        self.assertEqual(w._apply_source, 0)
        self.assertEqual(m.read_config(), saved)
        self.ctl.assert_called_once_with('kill', '-s', 'HUP', m.SERVICE)

    def test_invalid_or_missing_lut_does_not_change_config(self):
        cfg = {**m.DEFAULTS, **m.STRING_DEFAULTS, 'lut_preset': 'file', 'lut': '/missing/cube.file'}
        m.save_preset('Missing LUT', cfg)
        self.win._reload_presets('Missing LUT')
        self.win.on_load_preset(None)
        self.assertFalse(m.CONFIG_PATH.exists())
        m.PRESETS_PATH.write_text('corrupt')
        self.win.on_load_preset(None)
        self.assertFalse(m.CONFIG_PATH.exists())
        self.ctl.assert_not_called()

    def test_card_headers_spacing_and_no_importer(self):
        for section in self.win.group_expanders:
            self.assertIsInstance(section, m.ControlSection)
            self.assertGreaterEqual(section.body.get_margin_top(), 18)
            self.assertGreaterEqual(section.body.get_margin_end(), 20)
            self.assertIsInstance(section.badge, m.Gtk.CenterBox)
            self.assertEqual(section.sign.get_icon_name(), 'list-remove-symbolic' if section.get_expanded() else 'list-add-symbolic')
            before = section.get_expanded()
            section.toggle.emit('clicked')
            self.assertNotEqual(section.get_expanded(), before)
        self.assertNotIn('file', dict(self.win._luts))
        self.assertFalse(hasattr(self.win, 'on_choose_lut'))
        self.assertFalse(hasattr(self.win, 'lut_row'))

    def test_select_autoloads_every_factory_preset_once(self):
        self.assertEqual(len(self.win._preset_entries), 21)
        for index, (name, cfg) in enumerate(m.FACTORY_PRESETS.items(), 1):
            with self.subTest(preset=name):
                with patch.object(m, 'write_config', wraps=m.write_config) as writer:
                    self.win.preset_row.set_selected(index)
                    writer.assert_called_once_with(cfg)
                self.assertEqual(self.win._collect(), cfg)
                self.assertEqual(m.read_config(), cfg)
                self.assertEqual(self.win._apply_source, 0)
        self.assertFalse(m.PRESETS_PATH.exists())
        self.assertEqual(self.ctl.call_count, 20)

    def test_factory_presets_available_if_personal_file_is_corrupt(self):
        m.PRESETS_PATH.write_text('corrupt')
        self.win._reload_presets()
        self.win.preset_row.set_selected(1)
        self.assertEqual(m.read_config(), m.FACTORY_PRESETS['Natural'])
        self.assertEqual(m.PRESETS_PATH.read_text(), 'corrupt')

    def test_personal_name_can_match_factory_without_overwriting_it(self):
        changed = {**m.DEFAULTS, **m.STRING_DEFAULTS, 'gamma': 1.7}
        m.save_preset('Natural', changed)
        self.win._reload_presets('Natural')
        self.win.preset_row.set_selected(0)
        self.win.preset_row.set_selected(self.win._preset_entries.index(('user', 'Natural')))
        self.assertEqual(m.read_config(), changed)
        self.assertEqual(m.FACTORY_PRESETS['Natural']['gamma'], 1.0)
        self.win.preset_row.set_selected(1)
        self.assertEqual(m.read_config()['gamma'], 1.0)

    def test_refreshing_dropdown_never_applies_a_preset(self):
        changed = {**m.DEFAULTS, **m.STRING_DEFAULTS, 'gamma': 1.7}
        m.save_preset('Studio', changed)
        with patch.object(m, 'write_config', wraps=m.write_config) as writer:
            self.win._reload_presets('Studio')
            writer.assert_not_called()
            self.win.preset_row.set_selected(0)
            writer.assert_not_called()
            self.win.preset_row.set_selected(self.win._preset_entries.index(('user', 'Studio')))
            writer.assert_called_once_with(changed)
        self.assertFalse(hasattr(self.win, 'load_preset_button'))
        action = self.win.group_expanders[1].toggle.get_parent().get_last_child()
        self.assertEqual(action.get_label(), 'Save')

    def test_rendered_badges_are_centered_and_controls_have_right_inset(self):
        self.win.present()
        loop = m.GLib.MainLoop()
        m.GLib.timeout_add(300, lambda: (loop.quit(), False)[-1])
        loop.run()
        for section in self.win.group_expanders:
            with self.subTest(section=section.toggle.get_tooltip_text()):
                badge = section.badge
                self.assertEqual((badge.get_width(), badge.get_height()), (36, 36))
                ok, bounds = section.badge_icon.compute_bounds(badge)
                self.assertTrue(ok)
                self.assertAlmostEqual(bounds.get_x() + bounds.get_width()/2, badge.get_width()/2, delta=1)
                self.assertAlmostEqual(bounds.get_y() + bounds.get_height()/2, badge.get_height()/2, delta=1)
                ok, bounds = section.sign.compute_bounds(section)
                self.assertTrue(ok)
                self.assertGreaterEqual(section.get_width() - bounds.get_x() - bounds.get_width(), 20)
        previous, following = self.win.previous_preset_button, self.win.next_preset_button
        self.assertEqual((previous.get_width(), previous.get_height()), (following.get_width(), following.get_height()))
        self.assertGreaterEqual(previous.get_width(), 36)
        ok, left = previous.compute_bounds(self.win.preset_navigation)
        self.assertTrue(ok)
        ok, right = following.compute_bounds(self.win.preset_navigation)
        self.assertTrue(ok)
        self.assertGreaterEqual(right.get_x() - left.get_x() - left.get_width(), 6)


    def test_preset_arrows_browse_forward_and_back_without_wrapping(self):
        w = self.win
        self.assertFalse(w.previous_preset_button.get_sensitive())
        self.assertTrue(w.next_preset_button.get_sensitive())
        for index, cfg in enumerate(m.FACTORY_PRESETS.values(), 1):
            with patch.object(m, 'write_config', wraps=m.write_config) as writer:
                w.next_preset_button.emit('clicked')
                writer.assert_called_once_with(cfg)
            self.assertEqual(w.preset_row.get_selected(), index)
        self.assertFalse(w.next_preset_button.get_sensitive())
        with patch.object(m, 'write_config') as writer:
            w.next_preset_button.emit('clicked')
            writer.assert_not_called()
        for index in range(19, 0, -1):
            w.previous_preset_button.emit('clicked')
            self.assertEqual(w.preset_row.get_selected(), index)
        self.assertFalse(w.previous_preset_button.get_sensitive())
        with patch.object(m, 'write_config') as writer:
            w.previous_preset_button.emit('clicked')
            writer.assert_not_called()
        self.assertEqual(self.ctl.call_count, 39)

    def test_arrows_include_saved_presets_and_refresh_their_state(self):
        w = self.win
        cfg = {**m.DEFAULTS, **m.STRING_DEFAULTS, 'gamma': 1.7}
        m.save_preset('Studio', cfg)
        w._reload_presets()
        w.preset_row.set_selected(20)
        self.assertTrue(w.next_preset_button.get_sensitive())
        w.next_preset_button.emit('clicked')
        self.assertEqual(w._collect(), cfg)
        self.assertFalse(w.next_preset_button.get_sensitive())
        w.previous_preset_button.emit('clicked')
        self.assertEqual(w._collect(), m.FACTORY_PRESETS['Moonlight'])

    def test_close_flushes_pending_settings(self):
        self.win.gamma_row.set_value(1.6)
        self.assertNotEqual(self.win._apply_source, 0)
        self.win.on_close()
        self.assertEqual(m.read_config()['gamma'], 1.6)
        # Cleanup must not remove a timer twice.
        self.win._refresh_source = 0


if __name__ == '__main__':
    unittest.main(verbosity=2)
