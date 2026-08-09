"""The client's error handling, with ``_post`` stubbed. No network, no key.

Only the parts that have already gone wrong in a real run are covered, which for
this file is the size fallback: ``openai/gpt-image-2`` refuses a ``256x256``
request outright, and an ordinary Gunlok ground texture is 256x256.

Run it as a script:

    uv run python tests/test_openrouter.py
"""

import io
import json
import os
import sys
import urllib.error

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from gklightmap import openrouter  # noqa: E402

PNG = b"\x89PNG\r\n\x1a\n"          # not decoded anywhere in this file
REPLY = {"data": [{"b64_json": "aGk="}], "usage": {"cost": 0.02}}


def _http_error(code, message):
    body = json.dumps({"error": {"message": message}}).encode()
    return urllib.error.HTTPError("u", code, "boom", {}, io.BytesIO(body))


class Stub:
    """Records every body it is handed, and replays a scripted outcome list."""

    def __init__(self, *outcomes):
        self.outcomes = list(outcomes)
        self.bodies = []

    def __call__(self, body, key, timeout):
        self.bodies.append(dict(body))
        outcome = self.outcomes.pop(0)
        if isinstance(outcome, Exception):
            raise outcome
        return outcome


def _with_post(stub, **kwargs):
    original = openrouter._post
    openrouter._post = stub
    try:
        return openrouter.generate(
            "prompt", PNG, key="k", size=kwargs.pop("size", "256x256"), **kwargs)
    finally:
        openrouter._post = original


def test_a_rejected_size_is_retried_without_one():
    stub = Stub(_http_error(400, "Invalid size '256x256'. Requested resolution is "
                                 "below the current minimum"),
                REPLY)
    data, usage = _with_post(stub)
    assert data == b"hi", data
    assert usage["cost"] == 0.02
    assert len(stub.bodies) == 2, stub.bodies
    assert stub.bodies[0]["size"] == "256x256"
    assert "size" not in stub.bodies[1], "the retry still carried a size"


def test_an_unrelated_400_is_not_retried():
    """A malformed request retried three times just spends the timeout three times."""
    stub = Stub(_http_error(400, "model not found"), REPLY)
    try:
        _with_post(stub)
    except openrouter.OpenRouterError as exc:
        assert "model not found" in str(exc), exc
    else:
        raise AssertionError("a 400 naming no size was retried")
    assert len(stub.bodies) == 1, stub.bodies


def test_a_429_is_retried_with_the_size_intact():
    stub = Stub(_http_error(429, "rate limited"), REPLY)
    data, _ = _with_post(stub)
    assert data == b"hi"
    assert len(stub.bodies) == 2
    assert stub.bodies[1]["size"] == "256x256", "the retry dropped the size"


def test_a_reply_with_no_image_is_an_error():
    stub = Stub({"data": []})
    try:
        _with_post(stub)
    except openrouter.OpenRouterError as exc:
        assert "no image" in str(exc), exc
    else:
        raise AssertionError("an empty reply was accepted")


def test_a_data_url_reply_is_decoded():
    stub = Stub({"data": [{"url": "data:image/png;base64,aGk="}]})
    data, _ = _with_post(stub)
    assert data == b"hi", data


def test_the_reference_image_is_attached_as_a_data_url():
    stub = Stub(REPLY)
    _with_post(stub)
    ref = stub.bodies[0]["input_references"][0]
    assert ref["type"] == "image_url"
    assert ref["image_url"]["url"].startswith("data:image/png;base64,")


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for test in tests:
        try:
            test()
        except Exception as exc:  # noqa: BLE001 - a harness reports rather than dies
            failed += 1
            print("FAIL %s: %s" % (test.__name__, exc))
        else:
            print("ok   %s" % test.__name__)
    print("%d/%d passed" % (len(tests) - failed, len(tests)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
