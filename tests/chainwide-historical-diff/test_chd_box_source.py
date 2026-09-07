"""chd_box_source: the readers' box mapping from algod, an oracle state, or a dict."""
import base64
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from chd_box_source import (AlgodBoxSource, DictBoxSource, OracleBoxSource,  # noqa: E402
                            slot_map_from_boxes)
from chd_slot_reader import read_slot_map  # noqa: E402

PAGE = bytes(32) + (7).to_bytes(32, "big") + bytes(32 * 62)
SPARSE_NAME = b"s:" + (2 ** 200).to_bytes(32, "big")
BOXES = {b"p:" + bytes(8): PAGE, SPARSE_NAME: (9).to_bytes(32, "big"),
         b"@puya-sol/2:" + b"0" * 42: b"\x00\x00"}


class _Algod:
    def application_boxes(self, app_id):
        return {"boxes": [{"name": base64.b64encode(n).decode()} for n in BOXES]}

    def application_box_by_name(self, app_id, name):
        return {"value": base64.b64encode(BOXES[name]).decode()}


def test_algod_source_is_a_mapping_of_every_box():
    src = AlgodBoxSource(_Algod(), 1)
    assert dict(src) == BOXES and src.snapshot() == BOXES


def test_slot_map_matches_the_algod_slot_reader():
    assert slot_map_from_boxes(DictBoxSource(BOXES)) == read_slot_map(_Algod(), 1)
    assert slot_map_from_boxes(BOXES) == {1: (7).to_bytes(32, "big"),
                                          2 ** 200: (9).to_bytes(32, "big")}


def test_oracle_source_keeps_the_current_app_only():
    entries = [{"key": SPARSE_NAME.hex(), "bytes": "09"},
               {"app": 7001, "key": "aa", "bytes": "bb"}]
    assert dict(OracleBoxSource(entries)) == {SPARSE_NAME: b"\x09"}
    assert dict(OracleBoxSource(entries, app=7001)) == {b"\xaa": b"\xbb"}
