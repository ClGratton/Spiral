#!/usr/bin/env python3
"""Issue one fixed Spiral Editor scene/material action through its private mailbox."""

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


REQUEST_HEADER = "SpiralEditorControlRequest 2"
RECEIPT_HEADER = "SpiralEditorControlReceipt 2"
SESSION_HEADER = "SpiralEditorControlSession 2"
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
    if (state != "Ready" or request_schema != 2 or receipt_schema != 2
            or actions != "InspectMaterialSurface,SelectEntityPatchMaterialSurface,InspectEntity,SelectEntity,SetEntityTransform,SetTypedLight,SetProjectColorPipeline,SetViewportMainCameraPose"
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


def _quote(value: str, label: str = "expected entity name",
           maximum_bytes: int = 256) -> str:
    if (not value or len(value.encode("utf-8")) > maximum_bytes
            or any(ord(character) < 0x20 for character in value)):
        raise ControlError(
            f"{label} must be non-empty, <={maximum_bytes} bytes, and contain no controls")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def _float32(value: float, label: str) -> float:
    try:
        value = struct.unpack("=f", struct.pack("=f", float(value)))[0]
    except (OverflowError, TypeError, ValueError) as error:
        raise ControlError(f"{label} must be a finite float32 value") from error
    if not math.isfinite(value) or value < 0.0 or value > 1.0:
        raise ControlError(f"{label} must be finite and in [0,1]")
    return value


def _finite_float(value: object, label: str) -> float:
    try:
        parsed = float(value)
    except (TypeError, ValueError) as error:
        raise ControlError(f"{label} must be numeric") from error
    if not math.isfinite(parsed):
        raise ControlError(f"{label} must be finite")
    return parsed


def _float32_range(value: object, label: str, minimum: float | None = None,
                   maximum: float | None = None) -> float:
    parsed = _finite_float(value, label)
    try:
        parsed = struct.unpack("=f", struct.pack("=f", parsed))[0]
    except OverflowError as error:
        raise ControlError(f"{label} is outside float32 range") from error
    if not math.isfinite(parsed):
        raise ControlError(f"{label} must be finite")
    if minimum is not None and parsed < minimum:
        raise ControlError(f"{label} must be >= {minimum}")
    if maximum is not None and parsed > maximum:
        raise ControlError(f"{label} must be <= {maximum}")
    return parsed


def _parse_integer(value: str, label: str, minimum: int = -(1 << 63),
                   maximum: int = (1 << 63) - 1) -> int:
    try:
        parsed = int(value, 10)
    except ValueError as error:
        raise ControlError(f"{label} must be an integer") from error
    if parsed < minimum or parsed > maximum:
        raise ControlError(f"{label} is outside [{minimum},{maximum}]")
    return parsed


def _parse_transform(tokens: list[str], label: str) -> list[object]:
    if len(tokens) != 12:
        raise ControlError(f"{label} transform requires exactly twelve values")
    sectors = [_parse_integer(token, f"{label} sector") for token in tokens[:3]]
    local = [_finite_float(token, f"{label} local position") for token in tokens[3:6]]
    rotation = [_float32_range(token, f"{label} rotation") for token in tokens[6:9]]
    scale = [_float32_range(token, f"{label} scale", 0.0) for token in tokens[9:12]]
    if any(value <= 0.0 for value in scale):
        raise ControlError(f"{label} scale must be strictly positive")
    return [*sectors, *local, *rotation, *scale]


def _format_transform(tokens: list[str], label: str, require_unit_scale: bool) -> tuple[str, list[object]]:
    values = _parse_transform(tokens, label)
    if require_unit_scale and values[9:] != [1.0, 1.0, 1.0]:
        raise ControlError(f"{label} main-camera scale must be exactly 1 1 1")
    formatted = [str(value) for value in values[:3]]
    formatted.extend(format(float(value), ".17g") for value in values[3:6])
    formatted.extend(format(float(value), ".9g") for value in values[6:])
    return " ".join(formatted), values


def _parse_light(tokens: list[str], label: str) -> list[object]:
    if len(tokens) != 10:
        raise ControlError(f"{label} light requires exactly ten values")
    light_type = tokens[0]
    if light_type not in ("Directional", "Point", "Spot"):
        raise ControlError(f"{label} light type is unsupported")
    color = [_float32_range(value, f"{label} light color", 0.0)
             for value in tokens[1:4]]
    photometric = _finite_float(tokens[4], f"{label} photometric value")
    unit = tokens[5]
    expected_unit = "Lux" if light_type == "Directional" else "Lumens"
    maximum = 1_000_000_000.0 if light_type == "Directional" else 10_000_000.0
    if unit != expected_unit or photometric < 0.0 or photometric > maximum:
        raise ControlError(
            f"{label} {light_type} light requires {expected_unit} in [0,{maximum:g}]")
    light_range = _float32_range(tokens[6], f"{label} light range", 0.0)
    inner = _float32_range(tokens[7], f"{label} inner cone", 0.0, 180.0)
    outer = _float32_range(tokens[8], f"{label} outer cone", inner, 180.0)
    shadows = _parse_bool(tokens[9], f"{label} casts-shadows")
    return [light_type, *color, photometric, unit, light_range, inner, outer, shadows]


def _format_light(tokens: list[str], label: str) -> tuple[str, list[object]]:
    values = _parse_light(tokens, label)
    formatted = [str(values[0])]
    formatted.extend(format(float(value), ".9g") for value in values[1:4])
    formatted.extend([format(float(values[4]), ".17g"), str(values[5])])
    formatted.extend(format(float(value), ".9g") for value in values[6:9])
    formatted.append("yes" if values[9] else "no")
    return " ".join(formatted), values


def _parse_color_pipeline(tokens: list[str], label: str) -> list[object]:
    if len(tokens) != 7:
        raise ControlError(f"{label} color pipeline requires exactly seven values")
    manual_ev = _finite_float(tokens[0], f"{label} manual EV100")
    saturation = _finite_float(tokens[1], f"{label} saturation")
    contrast = _finite_float(tokens[2], f"{label} contrast")
    mode = tokens[3]
    aperture = _finite_float(tokens[4], f"{label} aperture")
    shutter = _finite_float(tokens[5], f"{label} shutter")
    iso = _finite_float(tokens[6], f"{label} ISO")
    if not -16.0 <= manual_ev <= 16.0:
        raise ControlError(f"{label} manual EV100 must be in [-16,16]")
    if not 0.0 <= saturation <= 2.0 or not 0.0 <= contrast <= 2.0:
        raise ControlError(f"{label} saturation and contrast must be in [0,2]")
    if mode not in ("ManualEV100", "CameraCalibration"):
        raise ControlError(f"{label} exposure mode is unsupported")
    if not 0.7 <= aperture <= 64.0:
        raise ControlError(f"{label} aperture must be in [0.7,64]")
    if not 1.0 / 8000.0 <= shutter <= 60.0:
        raise ControlError(f"{label} shutter must be in [1/8000,60]")
    if not 1.0 <= iso <= 102400.0:
        raise ControlError(f"{label} ISO must be in [1,102400]")
    effective_ev = (math.log2((aperture * aperture) / shutter * (100.0 / iso))
                    if mode == "CameraCalibration" else manual_ev)
    if not math.isfinite(effective_ev) or not -16.0 <= effective_ev <= 16.0:
        raise ControlError(f"{label} effective EV100 must be in [-16,16]")
    return [manual_ev, saturation, contrast, mode, aperture, shutter, iso]


def _format_color_pipeline(tokens: list[str], label: str) -> tuple[str, list[object]]:
    values = _parse_color_pipeline(tokens, label)
    formatted = [format(float(value), ".17g") for value in values[:3]]
    formatted.append(str(values[3]))
    formatted.extend(format(float(value), ".17g") for value in values[4:])
    return " ".join(formatted), values


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
                   project_path: str, digest: str, expected_action: str,
                   allow_request_id_conflict: bool = False) -> dict[str, object]:
    lines = text.splitlines()
    if len(lines) != 53 or lines[0] != RECEIPT_HEADER:
        raise ControlError("unsupported or malformed editor-control receipt")
    keys = [
        ("RequestId", 1), ("SessionId", 1), ("ProjectPath", 1),
        ("RequestDigest", 1), ("Action", 1), ("Status", 1), ("Reason", 1),
        ("Frame", 1), ("Effect", 1), ("Recovery", 1), ("Persistence", 1),
        ("Saved", 1), ("EntityId", 1), ("EntityName", 1),
        ("MainCameraEntityId", 1), ("IsMainCamera", 1),
        ("SelectedEntityIdBefore", 1), ("SelectedEntityIdAfter", 1),
        ("MaterialHandle", 1), ("BeforeSurface", 5), ("AfterSurface", 5),
        ("BeforeTransform", 12), ("AfterTransform", 12),
        ("BeforeCameraPresent", 1), ("BeforeCamera", 7),
        ("AfterCameraPresent", 1), ("AfterCamera", 7),
        ("BeforeLightPresent", 1), ("BeforeLight", 10),
        ("AfterLightPresent", 1), ("AfterLight", 10),
        ("BeforeMeshRendererPresent", 1), ("BeforeMeshRenderer", 5),
        ("AfterMeshRendererPresent", 1), ("AfterMeshRenderer", 5),
        ("BeforeColorPipeline", 7), ("AfterColorPipeline", 7),
        ("AffectedEntityCount", 1), ("AffectedEntitySampleCount", 1),
        ("AffectedEntityIds", None), ("AffectedEntityIdsTruncated", 1),
        ("RendererGeneration", 1), ("UndoDepthBefore", 1), ("UndoDepthAfter", 1),
        ("RedoDepthBefore", 1), ("RedoDepthAfter", 1),
        ("SelectionCommitted", 1), ("PivotRetargeted", 1),
        ("RendererReadbackVerified", 1), ("PostconditionVerified", 1),
        ("RollbackVerified", 1), ("EditorCameraSynchronized", 1),
    ]
    values = {key: _parse_tokens(line, key, count)
              for line, (key, count) in zip(lines[1:], keys, strict=True)}
    if (values["RequestId"][0] != request_id
            or values["SessionId"][0] != session_id
            or values["ProjectPath"][0] != project_path):
        raise ControlError("receipt identity does not match the request/session/project")
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
    if (values["Persistence"][0] != "SessionOnly"
            or _parse_bool(values["Saved"][0], "saved")):
        raise ControlError("receipt unexpectedly claims persistent or saved state")
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
    def parse_camera(tokens: list[str], label: str) -> list[object]:
        return [_parse_bool(tokens[0], f"{label} primary"),
                *[_float32_range(value, label) for value in tokens[1:]]]

    def parse_mesh(tokens: list[str], label: str) -> list[object]:
        return [_parse_integer(tokens[0], f"{label} mesh asset", 0,
                               (1 << 64) - 1),
                _parse_integer(tokens[1], f"{label} material asset", 0,
                               (1 << 64) - 1),
                tokens[2], _parse_bool(tokens[3], f"{label} visible"),
                _parse_bool(tokens[4], f"{label} casts-shadows")]

    receipt = {
        "schema": 2,
        "requestId": request_id,
        "sessionId": session_id,
        "projectPath": project_path,
        "requestDigest": receipt_digest,
        "action": action,
        "status": status_value,
        "reason": values["Reason"][0],
        "frame": int(values["Frame"][0]),
        "effect": values["Effect"][0],
        "recovery": values["Recovery"][0],
        "entityId": int(values["EntityId"][0]),
        "entityName": values["EntityName"][0],
        "mainCameraEntityId": int(values["MainCameraEntityId"][0]),
        "isMainCamera": _parse_bool(values["IsMainCamera"][0], "main camera"),
        "selectedEntityIdBefore": int(values["SelectedEntityIdBefore"][0]),
        "selectedEntityIdAfter": int(values["SelectedEntityIdAfter"][0]),
        "materialHandle": int(values["MaterialHandle"][0]),
        "beforeSurface": before,
        "afterSurface": after,
        "beforeTransform": _parse_transform(values["BeforeTransform"], "receipt before"),
        "afterTransform": _parse_transform(values["AfterTransform"], "receipt after"),
        "beforeCameraPresent": _parse_bool(
            values["BeforeCameraPresent"][0], "before-camera presence"),
        "beforeCamera": parse_camera(values["BeforeCamera"], "receipt before camera"),
        "afterCameraPresent": _parse_bool(
            values["AfterCameraPresent"][0], "after-camera presence"),
        "afterCamera": parse_camera(values["AfterCamera"], "receipt after camera"),
        "beforeLightPresent": _parse_bool(
            values["BeforeLightPresent"][0], "before-light presence"),
        "beforeLight": _parse_light(values["BeforeLight"], "receipt before"),
        "afterLightPresent": _parse_bool(
            values["AfterLightPresent"][0], "after-light presence"),
        "afterLight": _parse_light(values["AfterLight"], "receipt after"),
        "beforeMeshRendererPresent": _parse_bool(
            values["BeforeMeshRendererPresent"][0], "before-mesh presence"),
        "beforeMeshRenderer": parse_mesh(
            values["BeforeMeshRenderer"], "receipt before mesh"),
        "afterMeshRendererPresent": _parse_bool(
            values["AfterMeshRendererPresent"][0], "after-mesh presence"),
        "afterMeshRenderer": parse_mesh(
            values["AfterMeshRenderer"], "receipt after mesh"),
        "beforeColorPipeline": _parse_color_pipeline(
            values["BeforeColorPipeline"], "receipt before"),
        "afterColorPipeline": _parse_color_pipeline(
            values["AfterColorPipeline"], "receipt after"),
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
        "postconditionVerified": _parse_bool(
            values["PostconditionVerified"][0], "postcondition"),
        "rollbackVerified": _parse_bool(values["RollbackVerified"][0], "rollback"),
        "editorCameraSynchronized": _parse_bool(
            values["EditorCameraSynchronized"][0], "editor-camera synchronization"),
    }
    if receipt["frame"] < 0:
        raise ControlError("receipt frame is invalid")
    for key in ("entityId", "mainCameraEntityId", "selectedEntityIdBefore",
                "selectedEntityIdAfter"):
        if not 0 <= int(receipt[key]) <= 0xFFFFFFFF:
            raise ControlError(f"receipt {key} is outside the entity-ID range")
    if not 0 <= int(receipt["materialHandle"]) <= 0xFFFFFFFFFFFFFFFF:
        raise ControlError("receipt material handle is outside its range")
    if (status_value == "Rejected" and receipt["effect"] == "RolledBack"
            and not receipt["rollbackVerified"]):
        raise ControlError("rolled-back rejection lacks rollback verification")
    if (status_value == "Rejected" and receipt["effect"] == "RecoveryRequired"
            and receipt["rollbackVerified"]):
        raise ControlError("recovery-required rejection falsely claims rollback verification")
    return receipt


def _wait_for_receipt(response: Path, recovery: Path, collision: Path,
                      request_id: str, session_id: str, project_path: str,
                      digest: str, action: str,
                      timeout_seconds: float) -> dict[str, object]:
    deadline = time.monotonic() + timeout_seconds
    deferred_response_error: ControlError | None = None
    while True:
        if recovery.exists():
            receipt = _parse_receipt(
                _read_private_regular(recovery, MAXIMUM_RECEIPT_BYTES),
                request_id, session_id, project_path, digest, action)
            if (receipt["status"] != "Rejected"
                    or receipt["effect"] != "RecoveryRequired"
                    or receipt["recovery"] != "RestartSession"):
                raise ControlError("recovery receipt has invalid semantics")
            return receipt
        if response.exists():
            try:
                return _parse_receipt(
                    _read_private_regular(response, MAXIMUM_RECEIPT_BYTES),
                    request_id, session_id, project_path, digest, action)
            except ControlError as error:
                deferred_response_error = error
        if collision.exists():
            return _parse_receipt(_read_private_regular(collision, MAXIMUM_RECEIPT_BYTES),
                                  request_id, session_id, project_path,
                                  digest, action, True)
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
    for name in ("inspect-entity", "select-entity"):
        action = subcommands.add_parser(name)
        action.add_argument("--entity-id", required=True, type=int)
        action.add_argument("--expected-name", required=True)
        if name == "select-entity":
            action.add_argument("--expected-selected-entity-id", required=True, type=int)
    for name in ("set-transform", "set-viewport-main-camera-pose"):
        action = subcommands.add_parser(name)
        action.add_argument("--entity-id", required=True, type=int)
        action.add_argument("--expected-name", required=True)
        action.add_argument("--expected-transform", required=True, nargs=12)
        action.add_argument("--new-transform", required=True, nargs=12)
    action = subcommands.add_parser("set-typed-light")
    action.add_argument("--entity-id", required=True, type=int)
    action.add_argument("--expected-name", required=True)
    action.add_argument("--expected-light", required=True, nargs=10)
    action.add_argument("--new-light", required=True, nargs=10)
    action = subcommands.add_parser("set-project-color-pipeline")
    action.add_argument("--expected-color-pipeline", required=True, nargs=7)
    action.add_argument("--new-color-pipeline", required=True, nargs=7)
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
    project_path = str(session["project_path"])
    entity_actions = {"inspect", "set", "inspect-entity", "select-entity",
                      "set-transform", "set-viewport-main-camera-pose", "set-typed-light"}
    if args.command in entity_actions and (args.entity_id <= 0 or args.entity_id > 0xFFFFFFFF):
        raise ControlError("entity ID must be a positive numeric ID")
    if args.command in ("inspect", "set") and (args.material_handle <= 0 or args.material_handle > 0xFFFFFFFFFFFFFFFF):
        raise ControlError("material handle must be a positive numeric ID")
    if (args.command == "select-entity"
            and not 0 <= args.expected_selected_entity_id <= 0xFFFFFFFF):
        raise ControlError("expected selected entity ID is outside the numeric ID range")
    action_names = {"inspect": "InspectMaterialSurface", "set": "SelectEntityPatchMaterialSurface",
                    "inspect-entity": "InspectEntity", "select-entity": "SelectEntity",
                    "set-transform": "SetEntityTransform", "set-typed-light": "SetTypedLight",
                    "set-project-color-pipeline": "SetProjectColorPipeline",
                    "set-viewport-main-camera-pose": "SetViewportMainCameraPose"}
    action = action_names[args.command]
    lines = [
        REQUEST_HEADER,
        f"RequestId {_quote(request_id, 'request ID', 64)}",
        f"SessionId {_quote(session_id, 'session ID', 128)}",
        f"ProjectPath {_quote(project_path, 'project path', 4096)}",
        f"Action {action}",
    ]
    if args.command in entity_actions:
        lines.extend([f"EntityId {args.entity_id}", f"ExpectedEntityName {_quote(args.expected_name)}"])
    if args.command in ("inspect", "set"):
        lines.append(f"MaterialHandle {args.material_handle}")
    expected_surface: list[float] | None = None
    new_surface: list[float] | None = None
    expected_transform: list[object] | None = None
    new_transform: list[object] | None = None
    expected_light: list[object] | None = None
    new_light: list[object] | None = None
    expected_color_pipeline: list[object] | None = None
    new_color_pipeline: list[object] | None = None
    if args.command == "set":
        labels = ["base R", "base G", "base B", "metallic", "roughness"]
        expected = [*args.expected_base, args.expected_metallic, args.expected_roughness]
        new = [*args.new_base, args.new_metallic, args.new_roughness]
        expected_surface = [_float32(value, "expected surface") for value in expected]
        new_surface = [_float32(value, "new surface") for value in new]
        lines.extend([
            "ExpectedSurface " + _format_surface(expected, ["expected " + label for label in labels]),
            "NewSurface " + _format_surface(new, ["new " + label for label in labels]),
            "Scope SharedMaterial",
        ])
    if args.command == "select-entity":
        lines.append(f"ExpectedSelectedEntityId {args.expected_selected_entity_id}")
    if args.command in ("set-transform", "set-viewport-main-camera-pose"):
        camera_pose = args.command == "set-viewport-main-camera-pose"
        expected_text, expected_transform = _format_transform(
            args.expected_transform, "expected", camera_pose)
        new_text, new_transform = _format_transform(args.new_transform, "new", camera_pose)
        lines.extend(["ExpectedTransform " + expected_text,
                      "NewTransform " + new_text])
    if args.command == "set-typed-light":
        expected_text, expected_light = _format_light(args.expected_light, "expected")
        new_text, new_light = _format_light(args.new_light, "new")
        lines.extend(["ExpectedLight " + expected_text, "NewLight " + new_text])
    if args.command == "set-project-color-pipeline":
        expected_text, expected_color_pipeline = _format_color_pipeline(
            args.expected_color_pipeline, "expected")
        new_text, new_color_pipeline = _format_color_pipeline(
            args.new_color_pipeline, "new")
        lines.extend(["ExpectedColorPipeline " + expected_text,
                      "NewColorPipeline " + new_text])
    contents = ("\n".join(lines) + "\n").encode("utf-8")
    if len(contents) > MAXIMUM_REQUEST_BYTES:
        raise ControlError("request exceeds the editor mailbox limit")
    digest = _fnv1a64(contents)
    response = responses / f"{request_id}.response"
    recovery = responses / f"{request_id}.recovery.response"
    collision = responses / f"{request_id}.collision.response"

    if response.exists() or recovery.exists() or collision.exists():
        receipt = _wait_for_receipt(response, recovery, collision, request_id, session_id,
                                    project_path, digest, action, 0.001)
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
                                    project_path, digest, action, args.timeout_seconds)
    if receipt["status"] == "Succeeded":
        history_unchanged = (receipt["undoDepthAfter"] == receipt["undoDepthBefore"]
                             and receipt["redoDepthAfter"] == receipt["redoDepthBefore"])
        one_history_entry = (receipt["undoDepthAfter"]
                             == min(receipt["undoDepthBefore"] + 1, 128)
                             and receipt["redoDepthAfter"] == 0)
        transform_unchanged = receipt["beforeTransform"] == receipt["afterTransform"]
        camera_unchanged = (receipt["beforeCameraPresent"] == receipt["afterCameraPresent"]
                            and receipt["beforeCamera"] == receipt["afterCamera"])
        light_unchanged = (receipt["beforeLightPresent"] == receipt["afterLightPresent"]
                           and receipt["beforeLight"] == receipt["afterLight"])
        mesh_unchanged = (receipt["beforeMeshRendererPresent"]
                          == receipt["afterMeshRendererPresent"]
                          and receipt["beforeMeshRenderer"] == receipt["afterMeshRenderer"])
        color_unchanged = receipt["beforeColorPipeline"] == receipt["afterColorPipeline"]
        common_valid = (receipt["action"] == action and receipt["reason"] == "ok"
            and receipt["rendererGeneration"] > 0
            and receipt["postconditionVerified"] and not receipt["rollbackVerified"])
        if args.command in entity_actions:
            common_valid = (common_valid
                and receipt["entityId"] == args.entity_id
                and receipt["entityName"] == args.expected_name
                and receipt["isMainCamera"]
                    == (receipt["entityId"] == receipt["mainCameraEntityId"]))
            if args.command not in ("inspect", "set"):
                common_valid = (common_valid
                    and receipt["affectedEntityCount"] == 1
                    and receipt["affectedEntityIds"] == [args.entity_id]
                    and not receipt["affectedEntityIdsTruncated"])
        if args.command == "inspect":
            semantic_valid = (receipt["effect"] == "ReadOnly"
                and receipt["recovery"] == "None"
                and receipt["materialHandle"] == args.material_handle
                and receipt["beforeSurface"] == receipt["afterSurface"]
                and not receipt["selectionCommitted"]
                and not receipt["pivotRetargeted"]
                and receipt["rendererReadbackVerified"] and history_unchanged
                and transform_unchanged and camera_unchanged and light_unchanged
                and mesh_unchanged and color_unchanged
                and receipt["selectedEntityIdAfter"] == receipt["selectedEntityIdBefore"])
            common_valid = (common_valid and receipt["affectedEntityCount"] >= 1
                            and args.entity_id in receipt["affectedEntityIds"])
        elif args.command == "set":
            semantic_valid = (receipt["effect"] == "SharedMaterialSurfacePatched"
                and receipt["recovery"] == "UndoRedo"
                and receipt["materialHandle"] == args.material_handle
                and receipt["beforeSurface"] == expected_surface
                and receipt["afterSurface"] == new_surface
                and receipt["selectionCommitted"]
                and receipt["pivotRetargeted"]
                and receipt["rendererReadbackVerified"] and one_history_entry
                and transform_unchanged and camera_unchanged and light_unchanged
                and mesh_unchanged and color_unchanged
                and receipt["selectedEntityIdAfter"] == args.entity_id)
            common_valid = (common_valid and receipt["affectedEntityCount"] >= 1
                            and args.entity_id in receipt["affectedEntityIds"])
        elif args.command == "inspect-entity":
            semantic_valid = (receipt["effect"] == "ReadOnly"
                and receipt["recovery"] == "None" and history_unchanged
                and transform_unchanged and camera_unchanged and light_unchanged
                and mesh_unchanged and color_unchanged
                and not receipt["selectionCommitted"] and not receipt["pivotRetargeted"]
                and not receipt["rendererReadbackVerified"]
                and not receipt["editorCameraSynchronized"]
                and receipt["selectedEntityIdAfter"] == receipt["selectedEntityIdBefore"])
        elif args.command == "select-entity":
            semantic_valid = (receipt["effect"] == "EntitySelected"
                and receipt["recovery"] == "SelectPreviousEntity" and history_unchanged
                and transform_unchanged and camera_unchanged and light_unchanged
                and mesh_unchanged and color_unchanged
                and receipt["selectedEntityIdBefore"] == args.expected_selected_entity_id
                and receipt["selectedEntityIdAfter"] == args.entity_id
                and receipt["selectionCommitted"]
                and receipt["pivotRetargeted"] == (not receipt["isMainCamera"])
                and not receipt["rendererReadbackVerified"]
                and not receipt["editorCameraSynchronized"])
        elif args.command in ("set-transform", "set-viewport-main-camera-pose"):
            main_camera_pose = args.command == "set-viewport-main-camera-pose"
            semantic_valid = (receipt["effect"]
                    == ("ViewportMainCameraPoseSet" if main_camera_pose else "EntityTransformSet")
                and receipt["recovery"] == "UndoRedo" and one_history_entry
                and receipt["beforeTransform"] == expected_transform
                and receipt["afterTransform"] == new_transform
                and camera_unchanged and light_unchanged and mesh_unchanged
                and color_unchanged and not receipt["selectionCommitted"]
                and receipt["pivotRetargeted"]
                    == (receipt["selectedEntityIdBefore"] == args.entity_id
                        and not receipt["isMainCamera"])
                and receipt["selectedEntityIdAfter"] == receipt["selectedEntityIdBefore"]
                and receipt["editorCameraSynchronized"] == receipt["isMainCamera"]
                and (not main_camera_pose or receipt["isMainCamera"])
                and not receipt["rendererReadbackVerified"])
        elif args.command == "set-typed-light":
            semantic_valid = (receipt["effect"] == "TypedLightSet"
                and receipt["recovery"] == "UndoRedo" and one_history_entry
                and receipt["beforeLightPresent"] and receipt["afterLightPresent"]
                and receipt["beforeLight"] == expected_light
                and receipt["afterLight"] == new_light
                and transform_unchanged and camera_unchanged and mesh_unchanged
                and color_unchanged and not receipt["selectionCommitted"]
                and not receipt["pivotRetargeted"]
                and receipt["selectedEntityIdAfter"] == receipt["selectedEntityIdBefore"]
                and not receipt["rendererReadbackVerified"]
                and not receipt["editorCameraSynchronized"])
        else:
            semantic_valid = (receipt["entityId"] == 0 and receipt["entityName"] == ""
                and receipt["affectedEntityCount"] == 0
                and receipt["affectedEntityIds"] == []
                and receipt["effect"] == "ProjectColorPipelineSet"
                and receipt["recovery"] == "UndoRedo" and one_history_entry
                and receipt["beforeColorPipeline"] == expected_color_pipeline
                and receipt["afterColorPipeline"] == new_color_pipeline
                and receipt["selectedEntityIdAfter"] == receipt["selectedEntityIdBefore"]
                and not receipt["selectionCommitted"] and not receipt["pivotRetargeted"]
                and receipt["rendererReadbackVerified"]
                and not receipt["editorCameraSynchronized"])
        if not common_valid or not semantic_valid:
            raise ControlError("successful receipt failed action-specific semantic validation")
    receipt["editorProcessId"] = session["process_id"]
    print(json.dumps(receipt, separators=(",", ":"), sort_keys=True))
    return 0 if receipt["status"] == "Succeeded" else 2


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ControlError as error:
        print(f"EditorMaterialControlError: {error}", file=sys.stderr)
        raise SystemExit(1)
