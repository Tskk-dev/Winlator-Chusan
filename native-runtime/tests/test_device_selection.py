import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("device", Path(__file__).parents[1] / "tools/test_device.py")
device = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(device)


class DeviceSelectionTest(unittest.TestCase):
    def test_requires_authorized_device(self):
        for output in ("List of devices attached\n", "List of devices attached\nabc unauthorized\n",
                       "List of devices attached\nabc offline\n"):
            with self.subTest(output=output), self.assertRaises(ValueError):
                device.select_device(output)

    def test_single_device_is_selected(self):
        self.assertEqual(device.select_device("List of devices attached\nabc device\n"), "abc")

    def test_multiple_devices_require_explicit_selection(self):
        output = "List of devices attached\nabc device\ndef device\n"
        with self.assertRaises(ValueError):
            device.select_device(output)
        self.assertEqual(device.select_device(output, "def"), "def")
        with self.assertRaises(ValueError):
            device.select_device(output, "other")


if __name__ == "__main__":
    unittest.main()
