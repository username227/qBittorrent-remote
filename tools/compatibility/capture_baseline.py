#!/usr/bin/env python3
"""Capture read-only API evidence from the dedicated loopback compatibility lab.

Only the temporary authentication session is changed. No credentials, cookies,
raw preferences, or torrent names/paths are included in the report.
"""
import argparse
import getpass
import http.cookiejar
import json
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

BASE = "http://127.0.0.1:8080"
PROBES = ("app/version", "app/webapiVersion", "app/buildInfo",
          "app/preferences", "transfer/info", "transfer/speedLimitsMode",
          "torrents/info", "torrents/categories", "torrents/tags",
          "sync/maindata?rid=0")


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def decode_response_body(content_type, body):
    if content_type.split(";", 1)[0].strip().lower() == "application/json":
        try:
            return json.loads(body)
        except (ValueError, UnicodeError):
            pass
    return body.decode("utf-8", "replace")


def field_shape(value):
    # Nested object keys may be paths or other private user-defined names.
    if isinstance(value, (dict, list)):
        return {"redacted_type": type(value).__name__, "count": len(value)} if value else value
    return type(value).__name__


def shape(value):
    if isinstance(value, dict):
        return {k: field_shape(v) for k, v in value.items()}
    return field_shape(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expect", default="4.1.0")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error("Output exists; choose a new file")
    opener = urllib.request.build_opener(
        urllib.request.ProxyHandler({}),
        urllib.request.HTTPCookieProcessor(http.cookiejar.CookieJar()),
        NoRedirect())

    def request(endpoint, fields=None):
        data = None if fields is None else urllib.parse.urlencode(fields).encode()
        req = urllib.request.Request(BASE + "/api/v2/" + endpoint, data=data,
                                     headers={"Referer": BASE + "/"})
        try:
            with opener.open(req, timeout=10) as response:
                status = response.status
                content_type = response.headers.get("Content-Type", "")
                body = response.read()
        except urllib.error.HTTPError as error:
            return {"status": error.code}, None
        value = decode_response_body(content_type, body)
        return {"status": status, "content_type": content_type}, value

    login, result = request("auth/login", {
        "username": "admin", "password": getpass.getpass("Test WebUI password: ")})
    if login["status"] != 200 or result != "Ok.":
        parser.exit(1, "Login failed; no further requests sent\n")
    report = {"schema": 1, "expected_daemon": args.expect,
              "login": login, "responses": {}}
    try:
        meta, version = request("app/version")
        if meta["status"] != 200 or not isinstance(version, str) or version.lstrip("v") != args.expect:
            parser.exit(1, "Unexpected daemon version; no capture performed\n")
        # Do not capture a populated server. Empty synthetic lab only.
        meta, torrents = request("torrents/info")
        if meta["status"] != 200 or torrents != []:
            parser.exit(1, "Expected an empty torrent list; no capture performed\n")

        for endpoint in PROBES:
            meta, body = request(endpoint)
            if endpoint == "torrents/info" and body != []:
                parser.exit(1, "Torrent list changed; no capture performed\n")
            if body is not None:
                if endpoint == "app/preferences":
                    meta["shape"] = shape(body)
                elif endpoint in ("torrents/categories", "torrents/tags"):
                    # Names could be private, even on an otherwise empty server.
                    meta["shape"] = ("object" if isinstance(body, dict) else
                                     "array" if isinstance(body, list) else shape(body))
                    if isinstance(body, (dict, list)):
                        meta["count"] = len(body)
                elif endpoint.startswith("sync/"):
                    if isinstance(body, dict):
                        if body.get("torrents"):
                            parser.exit(1, "Torrent list changed; no capture performed\n")
                        for key, value in list(body.items()):
                            if key == "server_state":
                                body[key] = shape(value)
                            elif ((key == "rid" and type(value) is int) or
                                  (key == "full_update" and type(value) is bool)):
                                continue
                            else:
                                body[key] = field_shape(value)
                    else:
                        body = field_shape(body)
                    meta["body"] = body
                elif endpoint in ("app/version", "app/webapiVersion", "app/buildInfo",
                                  "transfer/speedLimitsMode", "torrents/info"):
                    meta["body"] = body
                else:
                    meta["shape"] = shape(body)
            report["responses"][endpoint] = meta
    finally:
        request("auth/logout", {})
    with args.output.open("x", encoding="utf-8") as out:
        json.dump(report, out, indent=2)
        out.write("\n")
    print(f"Captured {len(report['responses'])} probes to {args.output}")


if __name__ == "__main__":
    main()
