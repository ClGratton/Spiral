#!/usr/bin/env python3
"""Issue one fixed Spiral Editor material-control action through its private mailbox."""

from __future__ import annotations

import argparse
import ctypes
import errno
import json
import math
import os
from pathlib import Path
import shlex
import stat
import struct
import sys
import time
import uuid


REQUEST_HEADER = "SpiralEditorControlRequest 1"
RECEIPT_HEADER = "SpiralEditorControlReceipt 1"
SESSION_HEADER = "SpiralEditorControlSession 1"
MAXIMUM_REQUEST_BYTES = 16 * 1024
MAXIMUM_RECEIPT_BYTES = 64 * 1024


class ControlError(RuntimeError):
    pass


def _read_private_regular(path: Path, maximum: int) -> str:
    flags = os.O_RDONLY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    try:
        descriptor = os.open(path, flags)
    except OSError as error:
        if error.errno == errno.ELOOP:
            raise ControlError(f"refusing symlink: {path}") from error
        raise ControlError(f"could not open {path}: {error}") from error
    try:
        status = os.fstat(descriptor)
        if not stat.S_ISREG(status.st_mode):
            raise ControlError(f"not a regular file: {path}")
        if hasattr(os, "geteuid") and status.st_uid != os.geteuid():
            raise ControlError(f"file is not owned by this user: {path}")
        if stat.S_IMODE(status.st_mode) & 0o077:
            raise ControlError(f"file is not owner-only: {path}")
        if status.st_size > maximum:
            raise ControlError(f"file exceeds {maximum} bytes: {path}")
        chunks: list[bytes] = []
        count = 0
        while True:
            chunk = os.read(descriptor, min(4096, maximum + 1 - count))
            if not chunk:
                break
            chunks.append(chunk)
            count += len(chunk)
            if count > maximum:
                raise ControlError(f"file exceeds {maximum} bytes: {path}")
        if count != status.st_size:
            raise ControlError(f"file changed while being read: {path}")
    finally:
        os.close(descriptor)
    try:
        return b"".join(chunks).decode("utf-8")
    except UnicodeDecodeError as error:
        raise ControlError(f"file is not UTF-8 text: {path}") from error


def _validate_private_directory(path: Path) -> None:
    try:
        status = path.lstat()
    except OSError as error:
        raise ControlError(f"could not inspect directory {path}: {error}") from error
    if stat.S_ISLNK(status.st_mode) or not stat.S_ISDIR(status.st_mode):
        raise ControlError(f"not a private regular directory: {path}")
    if hasattr(os, "geteuid") and status.st_uid != os.geteuid():
        raise ControlError(f"directory is not owned by this user: {path}")
    if stat.S_IMODE(status.st_mode) & 0o077:
        raise ControlError(f"directory is not owner-only: {path}")


def _parse_tokens(line: str, key: str, count: int | None = None) -> list[str]:
    try:
        tokens = shlex.split(line, posix=True)
    except ValueError as error:
        raise ControlError(f"invalid quoted field {key}") from error
    if not tokens or tokens[0] != key or (count is not None and len(tokens) != count + 1):
        raise ControlError(f"invalid or out-of-order field {key}")
    return tokens[1:]


def _parse_session(control_dir: Path) -> dict[str, object]:
    lines = _read_private_regular(control_dir / "session.info", 4096).splitlines()
    if len(lines) != 12 or lines[0] != SESSION_HEADER:
        raise ControlError("unsupported or malformed editor-control session manifest")
    session_id = _parse_tokens(lines[1], "SessionId", 1)[0]
    state = _parse_tokens(lines[2], "State", 1)[0]
    process_id = int(_parse_tokens(lines[3], "ProcessId", 1)[0])
    project_path = _parse_tokens(lines[4], "ProjectPath", 1)[0]
    request_schema = int(_parse_tokens(lines[5], "RequestSchema", 1)[0])
    receipt_schema = int(_parse_tokens(lines[6], "ReceiptSchema", 1)[0])
    actions = _parse_tokens(lines[7], "Actions", 1)[0]
    maximum_request = int(_parse_tokens(lines[8], "MaximumRequestBytes", 1)[0])
    maximum_per_frame = int(_parse_tokens(lines[9], "MaximumRequestsPerFrame", 1)[0])
    maximum_terminal = int(_parse_tokens(lines[10], "MaximumTerminalRequests", 1)[0])
    maximum_affected = int(_parse_tokens(lines[11], "MaximumAffectedEntityIds", 1)[0])
    if (state != "Ready" or request_schema != 1 or receipt_schema != 1
            or actions != "InspectMaterialSurface,SelectEntityPatchMaterialSurface"
            or maximum_request != MAXIMUM_REQUEST_BYTES
            or maximum_per_frame != 4 or maximum_terminal != 256
            or maximum_affected != 32 or process_id <= 0 or not project_path):
        raise ControlError("editor-control session contract is not supported by this helper")
    return {"session_id": session_id, "state": state,
            "process_id": process_id, "project_path": project_path}


def _stable_id(value: str) -> str:
    if not value or value.startswith(".") or len(value) > 64 or any(
            not (character.isascii() and (character.isalnum() or character in "-_."))
            for character in value):
        raise ControlError(
            "request ID must not start with '.', followed by 1-63 ASCII letters, digits, '-', '_', or '.'")
    return value


def _quote(value: str) -> str:
    if (not value or len(value.encode("utf-8")) > 256
            or any(ord(character) < 0x20 for character in value)):
        raise ControlError("expected entity name must be non-empty, <=256 characters, and contain no controls")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _float32(value: float, label: str) -> float:
    value = struct.unpack("=f", struct.pack("=f", value))[0]
    if not math.isfinite(value) or value < 0.0 or value > 1.0:
        raise ControlError(f"{label} must be finite and in [0,1]")
    return value


def _format_surface(values: list[float], labels: list[str]) -> str:
    if len(values) != 5:
        raise ControlError("material surface requires exactly five values")
    return " ".join(format(_float32(value, label), ".9g")
                    for value, label in zip(values, labels, strict=True))


def _fnv1a64(contents: bytes) -> str:
    value = 14695981039346656037
    for byte in contents:
        value ^= byte
        value = (value * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return f"{value:016x}"


def _publish_no_replace(temporary: Path, destination: Path) -> None:
    if sys.platform.startswith("linux"):
        libc = ctypes.CDLL(None, use_errno=True)
        renameat2 = getattr(libc, "renameat2", None)
        if renameat2 is not None:
            renameat2.argtypes = [ctypes.c_int, ctypes.c_char_p,
                                  ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
            renameat2.restype = ctypes.c_int
            at_fdcwd = -100
            result = renameat2(at_fdcwd, os.fsencode(temporary), at_fdcwd,
                               os.fsencode(destination), 1)
            if result == 0:
                return
            native_error = ctypes.get_errno()
            if native_error == errno.EEXIST:
                raise FileExistsError(destination)
            if native_error not in (errno.ENOSYS, errno.EINVAL):
                raise OSError(native_error, os.strerror(native_error), destination)
    try:
        os.link(temporary, destination, follow_symlinks=False)
        temporary.unlink()
    except FileExistsError:
        raise


def _atomic_request(path: Path, contents: bytes) -> None:
    temporary = path.parent / f".{path.stem}.{uuid.uuid4().hex}.tmp"
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(temporary, flags, 0o600)
    try:
        offset = 0
        while offset < len(contents):
            offset += os.write(descriptor, contents[offset:])
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    try:
        _publish_no_replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    except BaseException:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
        raise


def _parse_bool(value: str, key: str) -> bool:
    if value == "yes":
        return True
    if value == "no":
        return False
    raise ControlError(f"invalid {key} boolean")


def _parse_receipt(text: str, request_id: str, session_id: str,
                   digest: str, expected_action: str,
                   allow_request_id_conflict: bool = False) -> dict[str, object]:
    lines = text.splitlines()
    if len(lines) != 27 or lines[0] != RECEIPT_HEADER:
        raise ControlError("unsupported or malformed editor-control receipt")
    keys = [
        ("RequestId", 1), ("SessionId", 1), ("RequestDigest", 1), ("Action", 1),
        ("Status", 1), ("Reason", 1), ("Frame", 1), ("Effect", 1),
        ("Recovery", 1), ("EntityId", 1), ("EntityName", 1),
        ("MaterialHandle", 1), ("BeforeSurface", 5), ("AfterSurface", 5),
        ("AffectedEntityCount", 1), ("AffectedEntitySampleCount", 1),
        ("AffectedEntityIds", None), ("AffectedEntityIdsTruncated", 1),
        ("RendererGeneration", 1), ("UndoDepthBefore", 1), ("UndoDepthAfter", 1),
        ("RedoDepthBefore", 1), ("RedoDepthAfter", 1),
        ("SelectionCommitted", 1), ("PivotRetargeted", 1),
        ("RendererReadbackVerified", 1),
    ]
    values = {key: _parse_tokens(line, key, count)
              for line, (key, count) in zip(lines[1:], keys, strict=True)}
    if values["RequestId"][0] != request_id or values["SessionId"][0] != session_id:
        raise ControlError("receipt identity does not match the request/session")
    receipt_digest = values["RequestDigest"][0]
    action = values["Action"][0]
    status_value = values["Status"][0]
    if status_value not in ("Succeeded", "Rejected"):
        raise ControlError("receipt has an invalid status")
    request_id_conflict = (allow_request_id_conflict
        and receipt_digest == "not-applicable"
        and action == "Unknown"
        and status_value == "Rejected"
        and values["Reason"][0] == "request_id_conflict"
        and values["Frame"][0] == "0"
        and values["Effect"][0] == "None"
        and values["Recovery"][0] == "None")
    if not request_id_conflict:
        if receipt_digest != digest:
            raise ControlError("request ID already belongs to a different payload")
        if action != expected_action:
            raise ControlError("receipt action does not match the request")
    affected_count = int(values["AffectedEntityCount"][0])
    affected_sample_count = int(values["AffectedEntitySampleCount"][0])
    affected_ids = [int(value) for value in values["AffectedEntityIds"]]
    affected_truncated = _parse_bool(
        values["AffectedEntityIdsTruncated"][0], "affected-entity truncation")
    if (affected_sample_count != len(affected_ids) or len(affected_ids) > 32
            or affected_count < affected_sample_count
            or affected_truncated != (affected_count > affected_sample_count)
            or affected_ids != sorted(set(affected_ids))):
        raise ControlError("receipt affected-entity summary is inconsistent or unbounded")
    before = [_float32(float(value), "receipt surface")
              for value in values["BeforeSurface"]]
    after = [_float32(float(value), "receipt surface")
             for value in values["AfterSurface"]]
    return {
        "schema": 1,
        "requestId": request_id,
        "sessionId": session_id,
        "requestDigest": receipt_digest,
        "action": action,
        "status": status_value,
        "reason": values["Reason"][0],
        "frame": int(values["Frame"][0]),
        "effect": values["Effect"][0],
        "recovery": values["Recovery"][0],
        "entityId": int(values["EntityId"][0]),
        "entityName": values["EntityName"][0],
        "materialHandle": int(values["MaterialHandle"][0]),
        "beforeSurface": before,
        "afterSurface": after,
        "affectedEntityCount": affected_count,
        "affectedEntityIds": affected_ids,
        "affectedEntityIdsTruncated": affected_truncated,
        "rendererGeneration": int(values["RendererGeneration"][0]),
        "undoDepthBefore": int(values["UndoDepthBefore"][0]),
        "undoDepthAfter": int(values["UndoDepthAfter"][0]),
        "redoDepthBefore": int(values["RedoDepthBefore"][0]),
        "redoDepthAfter": int(values["RedoDepthAfter"][0]),
        "selectionCommitted": _parse_bool(values["SelectionCommitted"][0], "selection"),
        "pivotRetargeted": _parse_bool(values["PivotRetargeted"][0], "pivot"),
        "rendererReadbackVerified": _parse_bool(
            values["RendererReadbackVerified"][0], "renderer readback"),
    }


def _wait_for_receipt(response: Path, recovery: Path, collision: Path,
                      request_id: str, session_id: str, digest: str, action: str,
                      timeout_seconds: float) -> dict[str, object]:
    deadline = time.monotonic() + timeout_seconds
    deferred_response_error: ControlError | None = None
    while True:
        if recovery.exists():
            receipt = _parse_receipt(
                _read_private_regular(recovery, MAXIMUM_RECEIPT_BYTES),
                request_id, session_id, digest, action)
            if (receipt["status"] != "Rejected"
                    or receipt["effect"] != "RecoveryRequired"
                    or receipt["recovery"] != "RestartSession"):
                raise ControlError("recovery receipt has invalid semantics")
            return receipt
        if response.exists():
            try:
                return _parse_receipt(
                    _read_private_regular(response, MAXIMUM_RECEIPT_BYTES),
                    request_id, session_id, digest, action)
            except ControlError as error:
                deferred_response_error = error
        if collision.exists():
            return _parse_receipt(_read_private_regular(collision, MAXIMUM_RECEIPT_BYTES),
                                  request_id, session_id, digest, action, True)
        if time.monotonic() >= deadline:
            if deferred_response_error is not None:
                raise deferred_response_error
            raise ControlError(f"timed out waiting for receipt {request_id}")
        time.sleep(0.02)


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--control-dir", required=True, type=Path)
    parser.add_argument("--expected-project", required=True, type=Path)
    parser.add_argument("--request-id", required=True)
    parser.add_argument("--timeout-seconds", type=float, default=5.0)
    subcommands = parser.add_subparsers(dest="command", required=True)
    for name in ("inspect", "set"):
        action = subcommands.add_parser(name)
        action.add_argument("--entity-id", required=True, type=int)
        action.add_argument("--expected-name", required=True)
        action.add_argument("--material-handle", required=True, type=int)
        if name == "set":
            action.add_argument("--expected-base", required=True, type=float, nargs=3)
            action.add_argument("--expected-metallic", required=True, type=float)
            action.add_argument("--expected-roughness", required=True, type=float)
            action.add_argument("--new-base", required=True, type=float, nargs=3)
            action.add_argument("--new-metallic", required=True, type=float)
            action.add_argument("--new-roughness", required=True, type=float)
    return parser


def main() -> int:
    args = _build_parser().parse_args()
    control_dir = args.control_dir
    if not control_dir.is_absolute():
        raise ControlError("--control-dir must be absolute")
    if not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0.0:
        raise ControlError("--timeout-seconds must be finite and positive")
    _validate_private_directory(control_dir)
    requests = control_dir / "requests"
    responses = control_dir / "responses"
    _validate_private_directory(requests)
    _validate_private_directory(responses)
    session = _parse_session(control_dir)
    session_id = str(session["session_id"])
    expected_project = args.expected_project.expanduser().resolve(strict=False)
    if not args.expected_project.is_absolute():
        raise ControlError("--expected-project must be absolute")
    if Path(str(session["project_path"])) != expected_project:
        raise ControlError(
            f"mailbox project mismatch: expected {expected_project}, got {session['project_path']}")
    request_id = _stable_id(args.request_id)
    if (args.entity_id <= 0 or args.entity_id > 0xFFFFFFFF
            or args.material_handle <= 0 or args.material_handle > 0xFFFFFFFFFFFFFFFF):
        raise ControlError("entity and material handles must be positive numeric IDs")
    expected_name = _quote(args.expected_name)
    action = ("InspectMaterialSurface" if args.command == "inspect"
              else "SelectEntityPatchMaterialSurface")
    lines = [
        REQUEST_HEADER,
        f'RequestId "{request_id}"',
        f'SessionId "{session_id}"',
        f"Action {action}",
        f"EntityId {args.entity_id}",
        f"ExpectedEntityName {expected_name}",
        f"MaterialHandle {args.material_handle}",
    ]
    if args.command == "set":
        labels = ["base R", "base G", "base B", "metallic", "roughness"]
        expected = [*args.expected_base, args.expected_metallic, args.expected_roughness]
        new = [*args.new_base, args.new_metallic, args.new_roughness]
        lines.extend([
            "ExpectedSurface " + _format_surface(expected, ["expected " + label for label in labels]),
            "NewSurface " + _format_surface(new, ["new " + label for label in labels]),
            "Scope SharedMaterial",
        ])
    contents = ("\n".join(lines) + "\n").encode("utf-8")
    if len(contents) > MAXIMUM_REQUEST_BYTES:
        raise ControlError("request exceeds the editor mailbox limit")
    digest = _fnv1a64(contents)
    response = responses / f"{request_id}.response"
    recovery = responses / f"{request_id}.recovery.response"
    collision = responses / f"{request_id}.collision.response"

    if response.exists() or recovery.exists() or collision.exists():
        receipt = _wait_for_receipt(response, recovery, collision, request_id, session_id,
                                    digest, action, 0.001)
    else:
        if (control_dir / "session.closed").exists():
            raise ControlError("editor-control session is closed")
        request = requests / f"{request_id}.request"
        try:
            _atomic_request(request, contents)
        except FileExistsError:
            existing = _read_private_regular(request, MAXIMUM_REQUEST_BYTES).encode("utf-8")
            if existing != contents:
                raise ControlError("request ID is already pending with a different payload")
        receipt = _wait_for_receipt(response, recovery, collision, request_id, session_id,
                                    digest, action, args.timeout_seconds)
    if receipt["status"] == "Succeeded":
        expected_before = None
        expected_after = None
        if args.command == "set":
            expected_before = [_float32(value, "expected surface") for value in expected]
            expected_after = [_float32(value, "new surface") for value in new]
        common_valid = (receipt["action"] == action
            and receipt["entityId"] == args.entity_id
            and receipt["entityName"] == args.expected_name
            and receipt["materialHandle"] == args.material_handle
            and args.entity_id in receipt["affectedEntityIds"]
            and receipt["affectedEntityCount"] >= 1
            and receipt["rendererGeneration"] > 0
            and receipt["rendererReadbackVerified"])
        if args.command == "inspect":
            semantic_valid = (receipt["effect"] == "ReadOnly"
                and receipt["recovery"] == "None"
                and receipt["beforeSurface"] == receipt["afterSurface"]
                and not receipt["selectionCommitted"]
                and not receipt["pivotRetargeted"]
                and receipt["undoDepthAfter"] == receipt["undoDepthBefore"]
                and receipt["redoDepthAfter"] == receipt["redoDepthBefore"])
        else:
            semantic_valid = (receipt["effect"] == "SharedMaterialSurfacePatched"
                and receipt["recovery"] == "UndoRedo"
                and receipt["beforeSurface"] == expected_before
                and receipt["afterSurface"] == expected_after
                and receipt["selectionCommitted"]
                and receipt["pivotRetargeted"]
                and receipt["undoDepthAfter"]
                    == min(receipt["undoDepthBefore"] + 1, 128)
                and receipt["redoDepthAfter"] == 0)
        if not common_valid or not semantic_valid:
            raise ControlError("successful receipt failed action-specific semantic validation")
    receipt["editorProcessId"] = session["process_id"]
    receipt["projectPath"] = session["project_path"]
    print(json.dumps(receipt, separators=(",", ":"), sort_keys=True))
    return 0 if receipt["status"] == "Succeeded" else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ControlError as error:
        print(f"EditorMaterialControlError: {error}", file=sys.stderr)
        raise SystemExit(1)
