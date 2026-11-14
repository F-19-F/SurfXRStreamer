"""Keep published firewall profiles consistent with the actual Rust endpoint contract."""
from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET

PROJECT = Path(__file__).resolve().parents[2] / "surfxr"
CRATES = PROJECT / "pc/surfxr-server/surfxr"
FIREWALL = CRATES / "xtask/firewall"


def ports(value):
    result = set()
    for item in value.split(","):
        pair = re.split("[:-]", item)
        first = int(pair[0])
        result.update(range(first, int(pair[-1]) + 1))
    return result


class EndpointContractTests(unittest.TestCase):
    def test_firewall_profiles_cover_the_private_endpoints_without_legacy_ports(self):
        contract = dict((k, int(v)) for k, v in re.findall(
            r"pub const (\w+): u16 = (\d+);", (CRATES / "common/src/ports.rs").read_text()))
        expected = {
            "tcp": {contract[n] for n in ("VR_CONTROL", "VR_MEDIA", "VR_PRIORITY", "DASHBOARD", "DESKTOP_CONTROL")},
            "udp": {contract[n] for n in ("VR_CONTROL", "VR_MEDIA", "VR_PRIORITY", "DESKTOP_CONTROL", "DESKTOP_MEDIA", "DESKTOP_PRIORITY")},
        }
        actual = {"tcp": set(), "udp": set()}
        for node in ET.parse(FIREWALL / "surfxr-firewalld.xml").findall("port"):
            actual[node.attrib["protocol"]].update(ports(node.attrib["port"]))
        self.assertEqual(actual, expected)
        profile = (FIREWALL / "ufw-surfxr").read_text().split("ports=", 1)[1].strip()
        self.assertEqual({protocol: ports(value) for value, protocol in
                          (group.split("/") for group in profile.split("|"))}, expected)
        script = (FIREWALL / "surfxr_fw_config.sh").read_text()
        tcp, udp = re.findall(r'ports="([0-9 ]+)"', script)
        self.assertEqual({"tcp": {int(v) for v in tcp.split()}, "udp": {int(v) for v in udp.split()}}, expected)
        self.assertTrue(all(not (set(range(9942, 9953)) | {8082}) & group for group in actual.values()))


if __name__ == "__main__":
    unittest.main()
