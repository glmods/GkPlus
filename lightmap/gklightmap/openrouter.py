"""One POST to OpenRouter's image endpoint, and the bytes that come back.

``POST https://openrouter.ai/api/v1/images`` with ``input_references`` is the
image-to-image route -- the same endpoint plain generation uses, with the source
image attached as a data URL. The chat-completions route can also return images on
some models; this one is used because the request and the response are both flat
and documented, and because the reply carries ``usage.cost`` so a run can say what
it spent.

The client is :mod:`urllib`. One endpoint, one POST, a JSON body -- a dependency
would buy retries, and the retries are below.
"""

import base64
import json
import os
import time
import urllib.error
import urllib.request

ENDPOINT = "https://openrouter.ai/api/v1/images"

#: Default model. An image *editing* model is required, not a text-to-image one:
#: the request carries the texture and the answer has to stay registered to it.
DEFAULT_MODEL = "google/gemini-3.1-flash-image"

#: Where a key comes from, in order. The environment first, then a file of that
#: name at the repository root -- which is where this checkout keeps one, and which
#: `.git/info/exclude` already keeps out of commits.
KEY_ENV = "OPENROUTER_API_KEY"
_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class OpenRouterError(RuntimeError):
    pass


def api_key():
    key = os.environ.get(KEY_ENV, "").strip()
    if key:
        return key
    path = os.path.join(_ROOT, KEY_ENV)
    if os.path.isfile(path):
        with open(path) as fh:
            key = fh.read().strip()
        if key:
            return key
    raise SystemExit(
        "no OpenRouter key: set %s, or put one in %s" % (KEY_ENV, path))


def _post(body, key, timeout):
    request = urllib.request.Request(
        ENDPOINT,
        data=json.dumps(body).encode(),
        headers={
            "Authorization": "Bearer " + key,
            "Content-Type": "application/json",
            # Optional attribution headers OpenRouter uses for its rankings. Named
            # after the tool so a bill can be read.
            "X-Title": "gklightmap",
        },
        method="POST")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode())


def _message(payload):
    """The human-readable half of an error body, whatever shape it arrives in."""
    if isinstance(payload, dict):
        error = payload.get("error")
        if isinstance(error, dict):
            return str(error.get("message") or error)
        if error:
            return str(error)
        if payload.get("message"):
            return str(payload["message"])
    return json.dumps(payload)[:400]


#: Statuses worth trying again: rate limiting, and the provider-side transients
#: OpenRouter documents (502/503/524/529). A 400 or a 401 is not one of them --
#: retrying a malformed request just spends the timeout three times.
RETRY_STATUS = frozenset({408, 429, 500, 502, 503, 504, 524, 529})


def generate(prompt, image_png, model=DEFAULT_MODEL, size=None, key=None,
             timeout=300, attempts=3, output_format="png"):
    """Ask for one image, with ``image_png`` attached as the reference.

    Returns ``(image bytes, usage dict)``. ``size`` is passed through verbatim when
    given -- OpenRouter takes either a tier (``"1K"``, ``"2K"``) or explicit pixels
    (``"1024x1024"``), and providers clamp to what they support, so an exact request
    is a preference rather than a guarantee. The caller resizes the answer back
    regardless; nothing downstream trusts the model to have honoured it.
    """
    key = key or api_key()
    body = {
        "model": model,
        "prompt": prompt,
        "n": 1,
        "output_format": output_format,
        "input_references": [{
            "type": "image_url",
            "image_url": {
                "url": "data:image/png;base64," + base64.b64encode(image_png).decode(),
            },
        }],
    }
    if size:
        body["size"] = size

    last = None
    for attempt in range(attempts):
        try:
            payload = _post(body, key, timeout)
            break
        except urllib.error.HTTPError as exc:
            detail = exc.read().decode("utf-8", "replace")
            try:
                detail = _message(json.loads(detail))
            except ValueError:
                detail = detail[:400]
            last = OpenRouterError("HTTP %d: %s" % (exc.code, detail))
            # **A rejected `size` is not a failed request, it is the wrong request.**
            # Every model has its own floor, ceiling and set of legal aspect ratios,
            # and OpenRouter does not publish them per endpoint in a form worth
            # encoding here -- `openai/gpt-image-2` refuses `256x256` outright
            # ("below the current minimum"), which is an ordinary size for this
            # game's textures. Dropping the field lets the provider pick; the reply
            # is resized back to the source either way, so the only thing lost is
            # the preference.
            if (exc.code == 400 and "size" in body
                    and ("size" in detail.lower() or "resolution" in detail.lower())):
                del body["size"]
                continue
            # **A reply that carried no image is a transient, not a refusal.** Gemini
            # answers a well-formed request with `finish_reason: STOP` and no image
            # data every so often - once in 35 on a Units run, and it happened to
            # land on the one sheet that mattered most. The request was accepted and
            # the model simply produced nothing, so asking again is the whole fix.
            # Deliberately narrow: a safety refusal is also a 400 and is NOT retried,
            # because looping around a provider's refusal is a different thing.
            if exc.code == 400 and "no image" in detail.lower():
                if attempt == attempts - 1:
                    raise last from exc
                continue
            if exc.code not in RETRY_STATUS or attempt == attempts - 1:
                raise last from exc
        except (urllib.error.URLError, TimeoutError) as exc:
            last = OpenRouterError("network: %s" % (exc,))
            if attempt == attempts - 1:
                raise last from exc
        time.sleep(2.0 * (attempt + 1))
    else:  # pragma: no cover - the loop always breaks or raises
        raise last

    data = payload.get("data") or []
    if not data:
        raise OpenRouterError("no image in the reply: " + _message(payload))
    entry = data[0]
    if entry.get("b64_json"):
        return base64.b64decode(entry["b64_json"]), payload.get("usage") or {}
    # Some providers hand back a URL instead. A data URL is decoded in place; an
    # http(s) one is fetched, because a caller wanting bytes should not have to
    # know which of the two it got.
    url = entry.get("url") or (entry.get("image_url") or {}).get("url") or ""
    if url.startswith("data:"):
        return base64.b64decode(url.split(",", 1)[1]), payload.get("usage") or {}
    if url.startswith("http"):
        with urllib.request.urlopen(url, timeout=timeout) as response:
            return response.read(), payload.get("usage") or {}
    raise OpenRouterError("reply carried neither b64_json nor a usable url")


def cost_of(usage):
    """The dollar cost of one reply, or ``None`` when the provider reported none."""
    if not isinstance(usage, dict):
        return None
    value = usage.get("cost")
    if value is None and isinstance(usage.get("usage"), dict):
        value = usage["usage"].get("cost")
    try:
        return float(value)
    except (TypeError, ValueError):
        return None
