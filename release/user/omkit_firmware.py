#!/usr/bin/env python3
"""OMKit firmware tool: gets the official camera firmware, checks it, applies the OMKit loader patch and writes two
update files for the memory card - the patched one (to install OMKit's loader) and the original one (to go back).

    python3 omkit_firmware.py [--firmware FILE] [--output DIR] [--yes]

Where the official file comes from, in this order:
  1. --firmware FILE, if given;
  2. a copy already in the output folder (output/original), from an earlier run;
  3. the official file under its own name (any letter case, also "name (1).BIN" and the like) next to this tool, in
     the release folder above it, or in the user's Downloads folder;
  4. a download from the manufacturer's update server, the way the manufacturer's updater asks for it.
Whatever the source, the file is used only if its size and SHA-256 equal those of the official file this release
supports (platform.json); the tool says which source it used.

What it sends to the update server (only in case 4): one HTTPS POST with the same XML request the manufacturer's updater
sends, carrying the camera model code and fixed values - a zero MAC address, serial number 000000000, the language ENU,
the operating system WIN, the current local time stamp - and the header "User-Agent: Olympus" that the updater uses;
then one HTTPS GET of the file link the server answers with. Nothing about this computer or its user is sent, apart
from what any connection shows (the IP address). A system proxy set in the environment is used.

It never writes to a memory card and never talks to a camera: it writes files into the output folder (default: output
next to this tool) and says what to copy where.

Python 3.9 or newer, standard library only. Platform data: platform.json and patch.json next to this file (or
--platform DIR).
"""
import argparse
import hashlib
import json
import os
import re
import ssl
import struct
import sys
import time
import urllib.error
import urllib.request
from html import unescape
from pathlib import Path
from xml.sax.saxutils import escape

MIN_PYTHON = (3, 9)
PATCH_FORMAT = 1
SERVER_URL = "https://fw01.om-digitalsolutions.net/ww_inform/fcache.do"
USER_AGENT = "Olympus"              # what the manufacturer's updater sends; the server is used as that updater uses it
TIMEOUT = 120                       # seconds, per network operation
MiB = 1 << 20

# --- firmware container ------------------------------------------------------------------------------------------
# A firmware file is a chain of components: header (32 bytes), payload, trailer (32 bytes). The payload is stored in
# groups of 16 bytes, each permuted and inverted; a tail shorter than 16 bytes is stored as is. The header and trailer
# hold the same first 28 bytes (magic "OE", model, flash address, payload length, version, kind); the trailer's last
# word is the checksum: the sum of all 32-bit little-endian words of header, decoded payload and trailer is 0 mod 2^32.

HEAD = 32
ORDER = (1, 3, 0, 2, 6, 4, 7, 5, 11, 10, 9, 8, 13, 14, 12, 15)    # decoded[16k + j] = ~stored[16k + ORDER[j]]
INVERSE = tuple(ORDER.index(i) for i in range(16))
_NOT = bytes(range(256))[::-1]                                     # translate table: b -> b ^ 0xff


class ToolError(Exception):
    """A check failed or something is missing: the message says what and what to do."""


def _permute(data, order):
    full = len(data) - len(data) % 16
    inverted = data[:full].translate(_NOT)
    out = bytearray(len(data))
    for j, src in enumerate(order):
        out[j:full:16] = inverted[src:full:16]
    out[full:] = data[full:]
    return bytes(out)


def decode_payload(stored):
    return _permute(stored, ORDER)


def encode_payload(payload):
    return _permute(payload, INVERSE)


def sum32(data):
    if len(data) % 4:
        raise ToolError("internal error: length not a multiple of 4")
    return sum(struct.unpack("<%dI" % (len(data) // 4), data)) & 0xFFFFFFFF


def parse_container(data):
    """Components of a firmware file as dicts (offset, kind, model, header, payload decoded, trailer). Every structural
    rule and every checksum is checked; ToolError on the first failure."""
    comps = []
    pos = 0
    while pos < len(data):
        if len(data) - pos < 2 * HEAD:
            raise ToolError("firmware file: truncated component at offset 0x%x" % pos)
        header = data[pos:pos + HEAD]
        length = struct.unpack_from("<I", header, 0x0C)[0]
        end = pos + HEAD + length
        if header[:2] != b"OE" or length == 0 or length % 16 or end + HEAD > len(data):
            raise ToolError("firmware file: bad component header at offset 0x%x" % pos)
        trailer = data[end:end + HEAD]
        if header[:28] != trailer[:28] or header[28:32] != b"\0\0\0\0":
            raise ToolError("firmware file: header and trailer differ at offset 0x%x" % pos)
        kind = struct.unpack_from("<H", header, 0x14)[0]
        payload = decode_payload(data[pos + HEAD:end]) if kind else data[pos + HEAD:end]
        if (sum32(header) + sum32(payload) + sum32(trailer)) & 0xFFFFFFFF:
            raise ToolError("firmware file: checksum of component %d (kind 0x%04x) is wrong" % (len(comps), kind))
        if comps and header[2:4] != comps[0]["header"][2:4]:
            raise ToolError("firmware file: components for different models")
        comps.append({"offset": pos, "kind": kind, "model": header[2:4].hex(), "header": header,
                      "payload": payload, "trailer": trailer})
        pos = end + HEAD
    if not comps:
        raise ToolError("firmware file: empty")
    return comps


def encode_component(comp):
    """Stored bytes of a component, with the checksum computed for its (possibly changed) payload."""
    trailer = bytearray(comp["trailer"])
    trailer[0x1C:0x20] = b"\0\0\0\0"
    total = sum32(comp["header"]) + sum32(comp["payload"]) + sum32(bytes(trailer))
    struct.pack_into("<I", trailer, 0x1C, (-total) & 0xFFFFFFFF)
    body = encode_payload(comp["payload"]) if comp["kind"] else comp["payload"]
    return comp["header"] + body + bytes(trailer)


# --- platform data and the patch -------------------------------------------------------------------------------------

def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_platform(folder):
    folder = Path(folder)
    try:
        platform = json.loads((folder / "platform.json").read_text(encoding="utf-8"))
        patch = json.loads((folder / "patch.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise ToolError("cannot read platform.json and patch.json in %s: %s" % (folder, e))
    if patch.get("format") != PATCH_FORMAT:
        raise ToolError("patch.json format %r is not supported by this tool" % patch.get("format"))
    official = platform["official_file"]
    if (patch["input"]["sha256"] != official["sha256"] or patch["input"]["size"] != official["size"]
            or (patch["platform"], patch["firmware"]) != (platform["platform"], platform["firmware"])):
        raise ToolError("platform.json and patch.json do not belong together")
    return platform, patch


def edit_bytes(edit):
    return bytes.fromhex(" ".join(edit["bytes"]))


def apply_patch(official, platform, patch):
    """The patched update file made from the official file's bytes. Every check of patch.json is made; ToolError if
    any fails, and then nothing is returned."""
    if len(official) != patch["input"]["size"] or sha256(official) != patch["input"]["sha256"]:
        raise ToolError("the firmware file is not the official %s (size or SHA-256 differ)"
                        % platform["official_file"]["name"])
    comps = parse_container(official)
    if comps[0]["model"] != platform["container_model"]:
        raise ToolError("the firmware file is for another camera model")
    by_kind = {}
    for c in comps:
        by_kind.setdefault(c["kind"], []).append(c)
    for spec in patch["components"]:
        kind = int(spec["kind"], 16)
        found = by_kind.get(kind, [])
        if len(found) != 1:
            raise ToolError("the firmware file has %d components of kind 0x%04x, expected 1" % (len(found), kind))
        comp = found[0]
        if len(comp["payload"]) != spec["payload_size"] or sha256(comp["payload"]) != spec["payload_sha256_before"]:
            raise ToolError("component 0x%04x is not the expected one" % kind)
        payload = bytearray(comp["payload"])
        taken = []
        for edit in spec["edits"]:
            at, new = int(edit["offset"], 16), edit_bytes(edit)
            if not new or at < 0 or at + len(new) > len(payload) or any(at < e and s < at + len(new) for s, e in taken):
                raise ToolError("patch.json: edit %r is out of range or overlaps another" % edit.get("name"))
            taken.append((at, at + len(new)))
            payload[at:at + len(new)] = new
        if sha256(bytes(payload)) != spec["payload_sha256_after"]:
            raise ToolError("component 0x%04x after the edits is not the expected one" % kind)
        comp["payload"] = bytes(payload)
    keep = [int(k, 16) for k in patch["output"]["components"]]
    out = b"".join(encode_component(c) for c in comps if c["kind"] in keep)
    back = parse_container(out)                       # every checksum of the result, the same way as the camera's
    if [c["kind"] for c in back] != [c["kind"] for c in comps if c["kind"] in keep]:
        raise ToolError("the patched file has the wrong components")
    if len(out) != patch["output"]["size"] or sha256(out) != patch["output"]["sha256"]:
        raise ToolError("the patched file is not the expected one (size or SHA-256 differ)")
    return out


# --- the official file: local copies -----------------------------------------------------------------------------

def _official_ok(data, platform):
    official = platform["official_file"]
    return len(data) == official["size"] and sha256(data) == official["sha256"]


def _name_matches(name, official_name):
    """The official name in any letter case, also with a copy mark a browser adds: "name (1).BIN", "name-1.BIN"."""
    stem, _, ext = official_name.rpartition(".")
    pattern = re.escape(stem) + r"(\s*\(\d+\)|[ _-]\d+|[ _-]copy)?\." + re.escape(ext)
    return re.fullmatch(pattern, name, re.IGNORECASE) is not None


def downloads_folder():
    return Path.home() / "Downloads"


def find_local(platform, folders, say):
    """(data, path) of the first file in folders that has the official name, size and SHA-256; None if there is none.
    Files with the name but other contents are reported and skipped."""
    official = platform["official_file"]
    seen = set()
    for folder in folders:
        try:
            names = sorted(os.listdir(folder))
        except OSError:
            continue
        for name in names:
            path = Path(folder) / name
            if not _name_matches(name, official["name"]) or not path.is_file():
                continue
            key = os.path.normcase(str(path.resolve()))
            if key in seen:
                continue
            seen.add(key)
            if path.stat().st_size != official["size"]:
                say("  skipped %s: not the official file (size differs)" % path)
                continue
            data = path.read_bytes()
            if _official_ok(data, platform):
                return data, path
            say("  skipped %s: not the official file (SHA-256 differs)" % path)
    return None


# --- the official file: the manufacturer's update server -------------------------------------------------------------

_DEMAND = ('<?xml version="1.0"?>\n<firmware_demand><revision>0001</revision>'
           "<mac_address>00-00-00-00-00-00</mac_address><time>{time}</time><language>ENU</language>"
           "<application>FWUT</application><type>0000</type><version>00005000</version><os>WIN</os>"
           "<country_code>0001</country_code><hardwares><firmup_mode>0</firmup_mode><hardware_info>"
           "<hard_code>{hard_code}</hard_code><firmware_version>00001000</firmware_version>"
           "<serial_no>000000000</serial_no><default_languages><code>ENU</code></default_languages>"
           "</hardware_info></hardwares></firmware_demand>")


def build_demand(hard_code):
    return _DEMAND.format(time=time.strftime("%Y%m%d%H%M%S"), hard_code=escape(hard_code)).encode("utf-8")


def parse_result(xml):
    """The <firmware_info> entries of the server's answer as dicts of their text fields."""
    errors = re.findall(r"<error_message>(.*?)</error_message>", xml, re.S)
    if errors:
        raise ToolError("the update server answered with an error: " + "; ".join(unescape(e) for e in errors))
    return [{k: unescape(v) for k, v in re.findall(r"<(\w+)>([^<]*)</\1>", block)}
            for block in re.findall(r"<firmware_info>(.*?)</firmware_info>", xml, re.S)]


def _version_text(v):
    v = v.zfill(4)
    return "%d.%s" % (int(v[0]), v[1:].rstrip("0") or "0")


def select_firmware(infos, platform):
    """The server's entry for this release's firmware; ToolError if the server offers another version or none.
    The server answers with the latest version of each model only: an older one cannot be asked for."""
    kind, model_id = platform["server_model"]["hardware_kind"], platform["server_model"]["model_id"]
    mine = [i for i in infos if i.get("hardware_kind") == kind and i.get("model_id") == model_id and i.get("url")]
    if not mine:
        raise ToolError("the update server has no firmware for the %s" % platform["camera"])
    info = mine[0]
    if info.get("version") != platform["firmware_version"]:
        raise ToolError(
            "the update server offers firmware %s for the %s, but this release supports only firmware %s.\n"
            "The server gives only the latest version, so this release cannot download its firmware any more.\n"
            "If you have the official %s file (for example from the manufacturer's desktop application or an earlier "
            "download), put it next to this tool or give it with --firmware FILE. Otherwise look for an OMKit release "
            "for firmware %s." % (_version_text(info.get("version", "")), platform["camera"],
                                  platform["firmware_version_text"], platform["official_file"]["name"],
                                  _version_text(info.get("version", ""))))
    official = platform["official_file"]
    if info.get("file_name") != official["name"] or int(info.get("file_size") or 0) != official["size"]:
        raise ToolError("the update server offers %s (%s bytes), expected %s (%d bytes)"
                        % (info.get("file_name"), info.get("file_size"), official["name"], official["size"]))
    return info


def _network_error(e, what):
    reason = getattr(e, "reason", e)
    if isinstance(reason, ssl.SSLCertVerificationError) or "CERTIFICATE_VERIFY_FAILED" in str(reason):
        return ToolError(
            "%s: the secure connection could not be verified (%s).\nOn macOS with Python from python.org run "
            "\"Install Certificates.command\" in the Python folder of Applications once, then try again." % (what, reason))
    if isinstance(e, urllib.error.HTTPError):
        return ToolError("%s: the server answered HTTP %d %s" % (what, e.code, e.reason))
    return ToolError("%s failed: %s. Check the internet connection and try again." % (what, reason))


def download(platform, server_url, say):
    """The official file from the update server, in memory; checked before it is returned."""
    request = urllib.request.Request(server_url, data=build_demand(platform["hard_code"]), method="POST",
                                     headers={"User-Agent": USER_AGENT, "Content-Type": "text/xml; Charset=UTF-8"})
    say("Asking the manufacturer's update server for the %s firmware..." % platform["camera"])
    try:
        with urllib.request.urlopen(request, timeout=TIMEOUT) as resp:
            xml = resp.read().decode("utf-8", errors="replace")
    except (urllib.error.URLError, OSError) as e:
        raise _network_error(e, "the request to the update server")
    info = select_firmware(parse_result(xml), platform)
    size = platform["official_file"]["size"]
    say("The server offers %s, version %s. Downloading %d MiB..." % (info["file_name"], _version_text(info["version"]),
                                                                    size // MiB))
    data = bytearray()
    try:
        with urllib.request.urlopen(urllib.request.Request(info["url"], headers={"User-Agent": USER_AGENT}),
                                    timeout=TIMEOUT) as resp:
            last = -1
            while True:
                chunk = resp.read(MiB)
                if not chunk:
                    break
                data += chunk
                if len(data) > size:
                    raise ToolError("the download is larger than the official file")
                pct = len(data) * 100 // size
                if pct != last:
                    last = pct
                    sys.stdout.write("\r  %3d%%  %d / %d MiB" % (pct, len(data) // MiB, size // MiB))
                    sys.stdout.flush()
    except (urllib.error.URLError, OSError) as e:
        sys.stdout.write("\n")
        raise _network_error(e, "the download")
    sys.stdout.write("\n")
    data = bytes(data)
    if not _official_ok(data, platform):
        raise ToolError("the downloaded file is not the expected official file (size or SHA-256 differ); nothing was "
                        "written. Try again later.")
    return data


# --- output ------------------------------------------------------------------------------------------------------

README_PATCHED = """OMKit loader for the {camera}, firmware {version}: the PATCHED update file.

{folder}/{file}   {size} bytes   SHA-256 {sha}

Install it (step by step: INSTALL.txt of the release, part A). Only on an OM-5 Mark II that runs EXACTLY
firmware {version}: flashing it over another version can brick the camera.
1. Use a memory card WITHOUT an OMKIT folder. Copy the folder {folder} from here to the root of the card, so that
   the card holds {folder}/{file}.
2. Battery fully charged. Switch the camera off, hold OK, switch it on while holding OK.
   The screen stays dark. Lamp lit STEADILY = flashing in progress: do not touch the camera, never switch off or
   open a cover. Lamp BLINKING = finished: now you may switch off.
3. Switch the camera off and on (do not hold OK): it must start normally. If it does not, go back to the stock
   firmware with the folder "original" next to this one.
4. Switch the camera off. Delete {folder}/{file} from the card (or format the card in the camera).
5. Copy the OMKIT folder of the release to the root of the card.

This file holds only the main code component of firmware {version} with the OMKit loader. It is made on your
computer from the official file; do not share it.
"""

README_ORIGINAL = """The OFFICIAL firmware {version} for the {camera}, unchanged: for going back to the stock firmware.

{folder}/{file}   {size} bytes   SHA-256 {sha}
(the manufacturer's file {official}, only renamed)

Go back to the stock firmware (step by step: INSTALL.txt of the release, part C):
1. Use a memory card WITHOUT an OMKIT folder. Copy the folder {folder} from here to the root of the card.
2. Battery fully charged. Switch the camera off, hold OK, switch it on while holding OK.
   The screen stays dark. Lamp lit STEADILY = flashing in progress: do not touch the camera, never switch off or
   open a cover. Lamp BLINKING = finished: now you may switch off.
3. Switch the camera off and on. Delete {folder}/{file} from the card.

Keep this file: once the manufacturer publishes a newer firmware, version {version} can no longer be downloaded.
Any later official firmware installed with the manufacturer's application also removes the OMKit loader.
"""


def _write_checked(path, data):
    """Write through a temporary file and read it back: the file is either complete and checked or not there."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data)
    if sha256(tmp.read_bytes()) != sha256(data):
        tmp.unlink()
        raise ToolError("writing %s failed (the file read back differs); check the disk" % path)
    os.replace(str(tmp), str(path))


def _write_set(root, data, readme, platform, extra):
    card = platform["card"]
    target = root / card["update_folder"] / card["update_file"]
    _write_checked(target, data)
    text = readme.format(camera=platform["camera"], version=platform["firmware_version_text"],
                         folder=card["update_folder"], file=card["update_file"], size=len(data), sha=sha256(data),
                         official=platform["official_file"]["name"], **extra)
    (root / "README.txt").write_text(text, encoding="utf-8", newline="\r\n")
    return target


def get_official(platform, output, firmware, search, server_url, say):
    """(data, source text) of the checked official file, from the sources in the order the module docstring gives."""
    official = platform["official_file"]
    if firmware is not None:
        path = Path(firmware)
        try:
            data = path.read_bytes()
        except OSError as e:
            raise ToolError("cannot read %s: %s" % (path, e))
        if not _official_ok(data, platform):
            raise ToolError("%s is not the official %s (size or SHA-256 differ)" % (path, official["name"]))
        return data, "the file given with --firmware: %s" % path
    card = platform["card"]
    cached = output / "original" / card["update_folder"] / card["update_file"]
    if cached.is_file():
        data = cached.read_bytes()
        if _official_ok(data, platform):
            return data, "the copy from an earlier run: %s" % cached
        say("  ignored %s: not the official file" % cached)
    if search:
        found = find_local(platform, search, say)
        if found:
            return found[0], "a file already on this computer: %s" % found[1]
    return download(platform, server_url, say), "a download from the manufacturer's update server"


def run(platform_dir, output, firmware=None, search=None, server_url=SERVER_URL, say=print):
    """Get, check, patch and write. Returns (patched path, original path). ToolError on any failure; then the
    patched file is not written."""
    platform, patch = load_platform(platform_dir)
    output = Path(output)
    data, source = get_official(platform, output, firmware, search or [], server_url, say)
    say("Official firmware %s %s: size and SHA-256 checked. Source: %s"
        % (platform["camera"], platform["firmware_version_text"], source))
    patched = apply_patch(data, platform, patch)
    say("Patch applied: every checksum and the SHA-256 of the result checked.")
    original = _write_set(output / "original", data, README_ORIGINAL, platform, {})
    target = _write_set(output / "patched", patched, README_PATCHED, platform, {})
    return target, original


DISCLAIMER = """OMKit firmware tool - {camera}, firmware {version}

This tool gets the official firmware {version} file, checks it and makes two update files for a memory card:
  patched   - the firmware with the OMKit loader, to install OMKit;
  original  - the official firmware unchanged, to go back.
It does not touch the camera or a memory card.

Installing modified firmware is AT YOUR OWN RISK. OMKit comes with NO WARRANTY; its authors accept no liability for
any damage to your camera or data. OMKit is not affiliated with or endorsed by the camera's manufacturer. Read
INSTALL.txt first.
"""


def main(argv=None):
    if sys.version_info < MIN_PYTHON:
        print("Python %d.%d or newer is needed, this is %s." % (MIN_PYTHON + (sys.version.split()[0],)))
        return 1
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Get the official firmware, check it and make the OMKit update files.")
    ap.add_argument("--firmware", metavar="FILE", help="use this official firmware file instead of searching for it "
                    "or downloading it (it is checked the same way)")
    ap.add_argument("--output", metavar="DIR", default=str(here / "output"),
                    help="where to write the files (default: output next to this tool)")
    ap.add_argument("--platform", metavar="DIR", default=str(here), help=argparse.SUPPRESS)
    ap.add_argument("--yes", action="store_true", help="do not wait for Enter before starting")
    args = ap.parse_args(argv)
    try:
        platform, _ = load_platform(args.platform)
        print(DISCLAIMER.format(camera=platform["camera"], version=platform["firmware_version_text"]))
        if not args.yes and sys.stdin is not None and sys.stdin.isatty():
            try:
                input("Press Enter to continue, or close this window to stop: ")
            except EOFError:                                    # no console input (e.g. NUL on Windows): go on
                print()
        search = [here, here.parent, downloads_folder()]
        target, original = run(args.platform, args.output, args.firmware, search)
    except ToolError as e:
        print("\nSTOPPED: %s\nThe patched file was not written." % e)
        return 2
    except KeyboardInterrupt:
        print("\nStopped by the user. The patched file was not written.")
        return 3
    card = platform["card"]
    print("\nDone.\n"
          "  Patched (to install OMKit):    %s\n"
          "  Original (to go back to stock): %s\n\n"
          "Next: copy the folder %s from %s to the root of a memory card without an OMKIT folder, then follow "
          "INSTALL.txt, part A, step 5." % (target, original, card["update_folder"], target.parent.parent))
    return 0


if __name__ == "__main__":
    sys.exit(main())
