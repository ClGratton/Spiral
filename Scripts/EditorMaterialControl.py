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


REQUEST_HEADER = "SpiralEditorControlRequest 4"
RECEIPT_HEADER = "SpiralEditorControlReceipt 4"
SESSION_HEADER = "SpiralEditorControlSession 4"
MAXIMUM_REQUEST_BYTES = 16 * 1024
MAXIMUM_RECEIPT_BYTES = 64 * 1024
MAXIMUM_ATTRIBUTION_BYTES = 2 * 1024
SESSION_ACTIONS = (
    "InspectMaterialSurface,SelectEntityPatchMaterialSurface,InspectEntity,SelectEntity,"
    "SetEntityTransform,SetTypedLight,SetProjectColorPipeline,SetViewportMainCameraPose,"
    "SetSceneDebugVisualization,SetMeshRendererFlags,InspectFabImport,SelectFabPackage,"
    "SetFabProvenance,ConfirmFabProvenance,CommitFabImport,CancelFabImport,DismissFabImport,"
    "PlaceMeshAsset,SetEntityMeshRendererAssets,SaveProjectState,ValidateProject,"
    "SetFabPanelVisible,InspectFabPanel")
FAB_STATES = ("none", "Idle", "Snapshotting", "Preparing", "AwaitingProvenance", "Cooking",
              "ReadyToCommit", "Committing", "Done", "Failed", "Cancelled", "FailedAfterCommit")
FAB_TERMINAL_STATES = ("Done", "Failed", "Cancelled", "FailedAfterCommit")
FAB_DECISIONS = ("ExactReuse", "AddNewStream", "ReplaceSameStreamSource",
                 "AddProductUpdateStream")
LICENSE_FAMILIES = ("FabStandard", "CC-BY", "LegacyUnrealMarketplace", "ReferenceOnly", "Unknown")
LICENSE_TIERS = ("Personal", "Professional", "NotApplicable", "Unknown")
METADATA_FLAGS = ("Unknown", "No", "Yes")
RAW_SOURCE_POLICIES = ("ExcludedFromProject", "PrivateProjectOnly", "Unknown")


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
    lines = _read_private_regular(control_dir / "session.info", 8192).splitlines()
    if len(lines) != 13 or lines[0] != SESSION_HEADER:
        raise ControlError(
            "editor-control schema 4 required; stale or malformed session manifest rejected")
    session_id = _parse_tokens(lines[1], "SessionId", 1)[0]
    state = _parse_tokens(lines[2], "State", 1)[0]
    process_id = int(_parse_tokens(lines[3], "ProcessId", 1)[0])
    project_path = _parse_tokens(lines[4], "ProjectPath", 1)[0]
    request_schema = int(_parse_tokens(lines[5], "RequestSchema", 1)[0])
    receipt_schema = int(_parse_tokens(lines[6], "ReceiptSchema", 1)[0])
    actions = _parse_tokens(lines[7], "Actions", 1)[0]
    fab_inbox = _parse_tokens(lines[8], "FabInbox", 1)[0]
    maximum_request = int(_parse_tokens(lines[9], "MaximumRequestBytes", 1)[0])
    maximum_per_frame = int(_parse_tokens(lines[10], "MaximumRequestsPerFrame", 1)[0])
    maximum_terminal = int(_parse_tokens(lines[11], "MaximumTerminalRequests", 1)[0])
    maximum_affected = int(_parse_tokens(lines[12], "MaximumAffectedEntityIds", 1)[0])
    if (state != "Ready" or request_schema != 4 or receipt_schema != 4
            or actions != SESSION_ACTIONS
            or maximum_request != MAXIMUM_REQUEST_BYTES
            or maximum_per_frame != 4 or maximum_terminal != 256
            or maximum_affected != 32 or process_id <= 0 or not project_path
            or Path(fab_inbox) != control_dir / "fab-inbox"):
        raise ControlError(
            "editor-control schema 4 contract required; stale or unsupported session rejected")
    return {"session_id": session_id, "state": state,
            "process_id": process_id, "project_path": project_path,
            "fab_inbox": fab_inbox}


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


def _none_text(token: str) -> str:
    return "" if token == "none" else token


def _parse_fab_block(values: dict[str, list[str]]) -> dict[str, object]:
    progress = [_parse_integer(token, "fab progress", 0, (1 << 64) - 1)
                for token in values["FabProgress"]]
    summary = [_parse_integer(token, "fab summary", 0, (1 << 64) - 1)
               for token in values["FabSummary"]]
    panel = values["FabPanel"]
    handles = lambda key: [_parse_integer(token, key, 0, (1 << 64) - 1) for token in values[key]]
    state = values["FabState"][0]
    if state not in FAB_STATES:
        raise ControlError("receipt has an unknown Fab state")
    result_handles = handles("FabResultHandles")
    if len(result_handles) > 32 or len(handles("FabProjectMeshAssets")) > 32 or len(
            handles("FabProjectMaterialAssets")) > 32:
        raise ControlError("receipt Fab handle sample is unbounded")
    return {
        "state": state,
        "jobId": _parse_integer(values["FabJobId"][0], "fab job", 0, (1 << 64) - 1),
        "cancelRequested": _parse_bool(values["FabCancelRequested"][0], "fab cancel"),
        "progress": {"files": progress[0], "fileCount": progress[1],
                     "bytes": progress[2], "byteCount": progress[3]},
        "sourceKind": values["FabSourceKind"][0],
        "sourceOrigin": values["FabSourceOrigin"][0],
        "sourceName": values["FabSourceName"][0],
        "errorCode": values["FabErrorCode"][0],
        "message": values["FabMessage"][0],
        "lastRejection": values["FabLastRejection"][0],
        "note": values["FabNote"][0],
        "format": values["FabFormat"][0],
        "sourceSha256": _none_text(values["FabSourceSha256"][0]),
        "expandedTreeSha256": _none_text(values["FabExpandedTreeSha256"][0]),
        "summary": {"vertices": summary[0], "triangles": summary[1], "primitives": summary[2],
                    "textures": summary[3], "files": summary[4], "bytes": summary[5]},
        "summaryMaterial": values["FabSummaryMaterial"][0],
        "provenanceValid": _parse_bool(values["FabProvenanceValid"][0], "fab provenance"),
        "provenanceConfirmed": _parse_bool(values["FabProvenanceConfirmed"][0], "fab confirm"),
        "provenanceDigest": _none_text(values["FabProvenanceDigest"][0]),
        "provenanceError": values["FabProvenanceError"][0],
        "relation": values["FabRelation"][0],
        "streamId": _none_text(values["FabStreamId"][0]),
        "generationId": _none_text(values["FabGenerationId"][0]),
        "projectChanged": _parse_bool(values["FabProjectChanged"][0], "fab project changed"),
        "assignmentApplied": _parse_bool(values["FabAssignmentApplied"][0], "fab assignment"),
        "commitOutcome": values["FabCommitOutcome"][0],
        "manifestRevision": _parse_integer(
            values["FabManifestRevision"][0], "fab revision", 0, (1 << 64) - 1),
        "manifestSha256": _none_text(values["FabManifestSha256"][0]),
        "meshAsset": _parse_integer(values["FabMeshAsset"][0], "fab mesh", 0, (1 << 64) - 1),
        "materialAsset": _parse_integer(
            values["FabMaterialAsset"][0], "fab material", 0, (1 << 64) - 1),
        "resultHandleCount": _parse_integer(
            values["FabResultHandleCount"][0], "fab result count", 0, (1 << 64) - 1),
        "resultHandles": result_handles,
        "projectReceiptCount": _parse_integer(
            values["FabProjectReceiptCount"][0], "fab receipt count", 0, (1 << 64) - 1),
        "projectMeshAssets": handles("FabProjectMeshAssets"),
        "projectMaterialAssets": handles("FabProjectMaterialAssets"),
        "projectStructural": values["FabProjectStructural"][0],
        "projectStructuralMessage": values["FabProjectStructuralMessage"][0],
        "projectValidation": values["FabProjectValidation"][0],
        "projectValidationMessage": values["FabProjectValidationMessage"][0],
        "panel": {
            "state": panel[0],
            "initialized": _parse_bool(panel[1], "panel initialized"),
            "failed": _parse_bool(panel[2], "panel failed"),
            "visible": _parse_bool(panel[3], "panel visible"),
            "keyboardOwnedByPage": _parse_bool(panel[4], "panel keyboard"),
            "textureValid": _parse_bool(panel[5], "panel texture"),
            "loading": _parse_bool(panel[6], "panel loading"),
            "framesReceived": _parse_integer(panel[7], "panel frames", 0, (1 << 64) - 1),
            "frameWidth": _parse_integer(panel[8], "panel width", 0, (1 << 32) - 1),
            "frameHeight": _parse_integer(panel[9], "panel height", 0, (1 << 32) - 1),
            "navigationDenials": _parse_integer(panel[10], "panel denials", 0, (1 << 32) - 1),
            "downloadsCompleted": _parse_integer(panel[11], "panel downloads", 0, (1 << 32) - 1),
            "host": values["FabPanelHost"][0],
            "error": values["FabPanelError"][0],
        },
    }


def _parse_receipt(text: str, request_id: str, session_id: str,
                   project_path: str, digest: str, expected_action: str,
                   allow_request_id_conflict: bool = False) -> dict[str, object]:
    lines = text.splitlines()
    if len(lines) != 98 or lines[0] != RECEIPT_HEADER:
        raise ControlError(
            "editor-control schema 4 required; stale or malformed receipt rejected")
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
        ("BeforeDebugVisualization", 2), ("AfterDebugVisualization", 2),
        ("AffectedEntityCount", 1), ("AffectedEntitySampleCount", 1),
        ("AffectedEntityIds", None), ("AffectedEntityIdsTruncated", 1),
        ("RendererGeneration", 1), ("DebugVisualizationGeneration", 1),
        ("UndoDepthBefore", 1), ("UndoDepthAfter", 1),
        ("RedoDepthBefore", 1), ("RedoDepthAfter", 1),
        ("SelectionCommitted", 1), ("PivotRetargeted", 1),
        ("RendererReadbackVerified", 1), ("PostconditionVerified", 1),
        ("RollbackVerified", 1), ("EditorCameraSynchronized", 1),
        ("FabState", 1), ("FabJobId", 1), ("FabCancelRequested", 1), ("FabProgress", 4),
        ("FabSourceKind", 1), ("FabSourceOrigin", 1), ("FabSourceName", 1),
        ("FabErrorCode", 1), ("FabMessage", 1), ("FabLastRejection", 1), ("FabNote", 1),
        ("FabFormat", 1), ("FabSourceSha256", 1), ("FabExpandedTreeSha256", 1),
        ("FabSummary", 6), ("FabSummaryMaterial", 1), ("FabProvenanceValid", 1),
        ("FabProvenanceConfirmed", 1), ("FabProvenanceDigest", 1), ("FabProvenanceError", 1),
        ("FabRelation", 1), ("FabStreamId", 1), ("FabGenerationId", 1),
        ("FabProjectChanged", 1), ("FabAssignmentApplied", 1), ("FabCommitOutcome", 1),
        ("FabManifestRevision", 1), ("FabManifestSha256", 1), ("FabMeshAsset", 1),
        ("FabMaterialAsset", 1), ("FabResultHandleCount", 1), ("FabResultHandles", None),
        ("FabProjectReceiptCount", 1), ("FabProjectMeshAssets", None),
        ("FabProjectMaterialAssets", None), ("FabProjectStructural", 1),
        ("FabProjectStructuralMessage", 1), ("FabProjectValidation", 1),
        ("FabProjectValidationMessage", 1), ("FabPanel", 12), ("FabPanelHost", 1),
        ("FabPanelError", 1),
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
    persistence = values["Persistence"][0]
    saved = _parse_bool(values["Saved"][0], "saved")
    if persistence not in ("SessionOnly", "Committed", "Saved") or (
            saved != (persistence != "SessionOnly")):
        raise ControlError("receipt persistence and saved state are inconsistent")
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

    def parse_debug_visualization(tokens: list[str], label: str) -> list[object]:
        if tokens[0] not in ("Lit", "MaterialId", "GeometricNormal", "ShadowCaster"):
            raise ControlError(f"invalid {label} debug view")
        return [tokens[0], _parse_bool(tokens[1], f"{label} selected bounds")]

    receipt = {
        "schema": 4,
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
        "persistence": persistence,
        "saved": saved,
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
        "beforeDebugVisualization": parse_debug_visualization(
            values["BeforeDebugVisualization"], "receipt before"),
        "afterDebugVisualization": parse_debug_visualization(
            values["AfterDebugVisualization"], "receipt after"),
        "affectedEntityCount": affected_count,
        "affectedEntityIds": affected_ids,
        "affectedEntityIdsTruncated": affected_truncated,
        "rendererGeneration": int(values["RendererGeneration"][0]),
        "debugVisualizationGeneration": int(
            values["DebugVisualizationGeneration"][0]),
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
        "fab": _parse_fab_block(values),
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


def _submit_request(control_dir: Path, session: dict[str, object], request_id: str,
                    action: str, lines: list[str], timeout_seconds: float) -> dict[str, object]:
    session_id = str(session["session_id"])
    project_path = str(session["project_path"])
    requests = control_dir / "requests"
    responses = control_dir / "responses"
    contents = ("\n".join(lines) + "\n").encode("utf-8")
    if len(contents) > MAXIMUM_REQUEST_BYTES:
        raise ControlError("request exceeds the editor mailbox limit")
    digest = _fnv1a64(contents)
    response = responses / f"{request_id}.response"
    recovery = responses / f"{request_id}.recovery.response"
    collision = responses / f"{request_id}.collision.response"

    if response.exists() or recovery.exists() or collision.exists():
        return _wait_for_receipt(response, recovery, collision, request_id, session_id,
                                 project_path, digest, action, 0.001)
    if (control_dir / "session.closed").exists():
        raise ControlError("editor-control session is closed")
    request = requests / f"{request_id}.request"
    try:
        _atomic_request(request, contents)
    except FileExistsError:
        existing = _read_private_regular(request, MAXIMUM_REQUEST_BYTES).encode("utf-8")
        if existing != contents:
            raise ControlError("request ID is already pending with a different payload")
    return _wait_for_receipt(response, recovery, collision, request_id, session_id,
                             project_path, digest, action, timeout_seconds)


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
    action = subcommands.add_parser("set-scene-debug-visualization")
    action.add_argument("--expected-selected-entity-id", required=True, type=int)
    action.add_argument("--expected-view", required=True,
                        choices=("Lit", "MaterialId", "GeometricNormal", "ShadowCaster"))
    action.add_argument("--expected-selected-bounds", required=True, choices=("yes", "no"))
    action.add_argument("--new-view", required=True,
                        choices=("Lit", "MaterialId", "GeometricNormal", "ShadowCaster"))
    action.add_argument("--new-selected-bounds", required=True, choices=("yes", "no"))
    action = subcommands.add_parser("set-mesh-renderer-flags")
    action.add_argument("--entity-id", required=True, type=int)
    action.add_argument("--expected-name", required=True)
    action.add_argument("--expected-visible", required=True, choices=("yes", "no"))
    action.add_argument("--expected-casts-shadows", required=True, choices=("yes", "no"))
    action.add_argument("--new-visible", required=True, choices=("yes", "no"))
    action.add_argument("--new-casts-shadows", required=True, choices=("yes", "no"))
    _add_fab_subcommands(subcommands)
    return parser

FAB_COMMANDS = {
    "inspect-fab": "InspectFabImport",
    "select-fab-package": "SelectFabPackage",
    "set-fab-provenance": "SetFabProvenance",
    "confirm-fab-provenance": "ConfirmFabProvenance",
    "commit-fab-import": "CommitFabImport",
    "cancel-fab-import": "CancelFabImport",
    "dismiss-fab-import": "DismissFabImport",
    "place-mesh-asset": "PlaceMeshAsset",
    "set-entity-mesh-renderer-assets": "SetEntityMeshRendererAssets",
    "save-project-state": "SaveProjectState",
    "validate-project": "ValidateProject",
    "set-fab-panel-visible": "SetFabPanelVisible",
    "inspect-fab-panel": "InspectFabPanel",
    "wait-fab-state": "InspectFabImport",
}


def _add_fab_subcommands(subcommands) -> None:
    action = subcommands.add_parser("inspect-fab")
    action.add_argument("--expected-job-id", type=int, default=None)
    action = subcommands.add_parser("select-fab-package")
    action.add_argument("--inbox-name", required=True)
    action.add_argument("--kind", required=True, choices=("zip", "glb", "gltf", "directory"))
    action.add_argument("--source-sha256", default=None)
    action = subcommands.add_parser("set-fab-provenance")
    action.add_argument("--expected-job-id", required=True, type=int)
    action.add_argument("--product-identity", required=True)
    action.add_argument("--product-name", required=True)
    action.add_argument("--publisher", required=True)
    action.add_argument("--version-label", required=True)
    action.add_argument("--license-family", required=True, choices=LICENSE_FAMILIES)
    action.add_argument("--license-tier", required=True, choices=LICENSE_TIERS)
    action.add_argument("--attribution-text", default=None)
    action.add_argument("--attribution-link", default=None)
    action.add_argument("--no-ai", required=True, choices=METADATA_FLAGS)
    action.add_argument("--generated-with-ai", required=True, choices=METADATA_FLAGS)
    action.add_argument("--raw-source-policy", required=True, choices=RAW_SOURCE_POLICIES)
    action = subcommands.add_parser("confirm-fab-provenance")
    action.add_argument("--expected-job-id", required=True, type=int)
    action.add_argument("--expected-provenance-digest", required=True)
    action = subcommands.add_parser("commit-fab-import")
    action.add_argument("--expected-job-id", required=True, type=int)
    action.add_argument("--expected-generation-id", required=True)
    action.add_argument("--expected-relation", required=True, choices=FAB_DECISIONS)
    action.add_argument("--assign-entity-id", type=int, default=None)
    action.add_argument("--assign-expected-name", default=None)
    action.add_argument("--assign-expected-mesh-asset", type=int, default=None)
    action.add_argument("--assign-expected-material-asset", type=int, default=None)
    for name in ("cancel-fab-import", "dismiss-fab-import"):
        action = subcommands.add_parser(name)
        action.add_argument("--expected-job-id", required=True, type=int)
    action = subcommands.add_parser("place-mesh-asset")
    action.add_argument("--mesh-asset", required=True, type=int)
    action.add_argument("--expected-selected-entity-id", required=True, type=int)
    action = subcommands.add_parser("set-entity-mesh-renderer-assets")
    action.add_argument("--entity-id", required=True, type=int)
    action.add_argument("--expected-name", required=True)
    action.add_argument("--expected-mesh-asset", required=True, type=int)
    action.add_argument("--expected-material-asset", required=True, type=int)
    action.add_argument("--new-mesh-asset", required=True, type=int)
    action.add_argument("--new-material-asset", required=True, type=int)
    action = subcommands.add_parser("save-project-state")
    action.add_argument("--expected-manifest-sha256", default=None)
    subcommands.add_parser("validate-project")
    action = subcommands.add_parser("set-fab-panel-visible")
    action.add_argument("--visible", required=True, choices=("yes", "no"))
    subcommands.add_parser("inspect-fab-panel")
    action = subcommands.add_parser("wait-fab-state")
    action.add_argument("--state", nargs="+", default=None, choices=FAB_STATES[1:])
    action.add_argument("--validation", nargs="+", default=None,
                        choices=("passed", "failed"))
    action.add_argument("--timeout", type=float, default=30.0)
    action.add_argument("--expected-job-id", type=int, default=None)


def _is_hex64(value: str) -> bool:
    return len(value) == 64 and all(character in "0123456789abcdef" for character in value)


def _handle(value: int, label: str, allow_zero: bool = False) -> int:
    if (value < 0 or value > 0xFFFFFFFFFFFFFFFF or (value == 0 and not allow_zero)):
        raise ControlError(f"{label} must be a positive 64-bit asset handle")
    return value


def _job_id(value: int | None, label: str = "expected job ID") -> int:
    if value is None or value <= 0 or value > 0xFFFFFFFFFFFFFFFF:
        raise ControlError(f"{label} must be a positive numeric ID")
    return value


def _fab_text(value: str, label: str, maximum_bytes: int, required: bool = True) -> str:
    if required and not value:
        raise ControlError(f"{label} must not be empty")
    if (len(value.encode("utf-8")) > maximum_bytes
            or any(ord(character) < 0x20 or ord(character) == 0x7F for character in value)):
        raise ControlError(f"{label} must be <={maximum_bytes} bytes and contain no controls")
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def provenance_digest(fields: dict[str, str]) -> str:
    """Mirror of Fab::ComputeFabProvenanceDigest for client-side verification."""
    import hashlib
    canonical = "SpiralFabProvenanceV1\n"
    for key in ("ProductIdentity", "ProductName", "Publisher", "VersionOrDownloadLabel",
                "LicenseFamily", "LicenseTier", "AttributionText", "AttributionLink",
                "NoAI", "GeneratedWithAI", "RawSourcePolicy"):
        value = fields[key]
        canonical += f"{len(value.encode('utf-8'))}:{value}\n"
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


def _build_fab_request(args) -> tuple[list[str], dict[str, object]]:
    """Returns the action-specific request lines and the client's expectations."""
    lines: list[str] = []
    expect: dict[str, object] = {}
    command = args.command
    if command == "inspect-fab":
        if args.expected_job_id is not None:
            expect["job"] = _job_id(args.expected_job_id)
            lines.append(f"ExpectedFabJobId {expect['job']}")
    elif command == "select-fab-package":
        name = args.inbox_name
        if (not name or len(name) > 128 or name.startswith(".") or ".." in name
                or not all(character.isascii() and (character.isalnum() or character in "._-")
                           for character in name)):
            raise ControlError(
                "inbox name must be one leaf of 1-128 ASCII letters, digits, '.', '_' or '-', "
                "with no '..' and no leading '.'")
        lines.append(f"InboxName {_fab_text(name, 'inbox name', 128)}")
        lines.append(f"ExpectedSourceKind {args.kind}")
        expect["kind"] = args.kind
        if args.source_sha256 is not None:
            if not _is_hex64(args.source_sha256):
                raise ControlError("source SHA-256 must be 64 lowercase hex digits")
            lines.append(f"ExpectedSourceSha256 {args.source_sha256}")
    elif command == "set-fab-provenance":
        expect["job"] = _job_id(args.expected_job_id)
        identity = args.product_identity
        listing = identity.removeprefix("https://www.fab.com/listings/")
        if (not identity.startswith("https://www.fab.com/listings/") or not listing
                or len(listing) > 64
                or not all(character.isascii() and (character.isalnum() or character == "-")
                           for character in listing)):
            raise ControlError(
                "product identity must be the canonical https://www.fab.com/listings/<id> URL")
        attribution_text = args.attribution_text or ""
        if len(attribution_text.encode("utf-8")) > MAXIMUM_ATTRIBUTION_BYTES:
            raise ControlError("attribution text exceeds the 2048-byte mailbox cap")
        attribution_link = args.attribution_link or ""
        if attribution_link and not attribution_link.startswith("https://"):
            raise ControlError("attribution link must be an https URL")
        if args.license_family == "CC-BY" and (not attribution_text or not attribution_link):
            raise ControlError("CC-BY requires attribution text and an https attribution link")
        if args.license_family == "ReferenceOnly":
            raise ControlError("Reference-only licenses cannot be imported")
        if args.license_family == "FabStandard" and args.license_tier not in (
                "Personal", "Professional"):
            raise ControlError("Fab Standard requires the Personal or Professional tier")
        fields = {
            "ProductIdentity": identity, "ProductName": args.product_name,
            "Publisher": args.publisher, "VersionOrDownloadLabel": args.version_label,
            "LicenseFamily": args.license_family, "LicenseTier": args.license_tier,
            "AttributionText": attribution_text, "AttributionLink": attribution_link,
            "NoAI": args.no_ai, "GeneratedWithAI": args.generated_with_ai,
            "RawSourcePolicy": args.raw_source_policy,
        }
        # The Editor normalizes the tier of a non-Fab-Standard license.
        if args.license_family not in ("FabStandard", "Unknown"):
            fields["LicenseTier"] = "NotApplicable"
        expect["digest"] = provenance_digest(fields)
        lines.append(f"ExpectedFabJobId {expect['job']}")
        lines.append(f"ProductIdentity {_fab_text(identity, 'product identity', 512)}")
        lines.append(f"ProductName {_fab_text(args.product_name, 'product name', 256)}")
        lines.append(f"Publisher {_fab_text(args.publisher, 'publisher', 256)}")
        lines.append(f"VersionOrDownloadLabel {_fab_text(args.version_label, 'version label', 128)}")
        lines.append(f"LicenseFamily {args.license_family}")
        lines.append(f"LicenseTier {args.license_tier}")
        if attribution_text:
            lines.append("AttributionText " + _fab_text(
                attribution_text, 'attribution text', MAXIMUM_ATTRIBUTION_BYTES))
        if attribution_link:
            lines.append("AttributionLink " + _fab_text(attribution_link, 'attribution link', 512))
        lines.append(f"NoAI {args.no_ai}")
        lines.append(f"GeneratedWithAI {args.generated_with_ai}")
        lines.append(f"RawSourcePolicy {args.raw_source_policy}")
    elif command == "confirm-fab-provenance":
        expect["job"] = _job_id(args.expected_job_id)
        if not _is_hex64(args.expected_provenance_digest):
            raise ControlError("expected provenance digest must be 64 lowercase hex digits")
        lines.append(f"ExpectedFabJobId {expect['job']}")
        lines.append(f"ExpectedProvenanceDigest {args.expected_provenance_digest}")
    elif command == "commit-fab-import":
        expect["job"] = _job_id(args.expected_job_id)
        if not _is_hex64(args.expected_generation_id):
            raise ControlError("expected generation ID must be 64 lowercase hex digits")
        expect["generation"] = args.expected_generation_id
        expect["relation"] = args.expected_relation
        lines.append(f"ExpectedFabJobId {expect['job']}")
        lines.append(f"ExpectedGenerationId {args.expected_generation_id}")
        lines.append(f"ExpectedRelation {args.expected_relation}")
        group = (args.assign_entity_id, args.assign_expected_name,
                 args.assign_expected_mesh_asset, args.assign_expected_material_asset)
        if any(value is not None for value in group):
            if any(value is None for value in group):
                raise ControlError(
                    "an assignment needs --assign-entity-id, --assign-expected-name, "
                    "--assign-expected-mesh-asset and --assign-expected-material-asset together")
            if args.assign_entity_id <= 0 or args.assign_entity_id > 0xFFFFFFFF:
                raise ControlError("assigned entity ID must be a positive numeric ID")
            expect["assignment"] = True
            lines.append(f"EntityId {args.assign_entity_id}")
            lines.append(f"ExpectedEntityName {_quote(args.assign_expected_name)}")
            lines.append("ExpectedMeshAsset " + str(
                _handle(args.assign_expected_mesh_asset, 'expected mesh asset', True)))
            lines.append("ExpectedMaterialAsset " + str(
                _handle(args.assign_expected_material_asset, 'expected material asset', True)))
    elif command in ("cancel-fab-import", "dismiss-fab-import"):
        expect["job"] = _job_id(args.expected_job_id)
        lines.append(f"ExpectedFabJobId {expect['job']}")
    elif command == "place-mesh-asset":
        expect["mesh"] = _handle(args.mesh_asset, "mesh asset")
        if not 0 <= args.expected_selected_entity_id <= 0xFFFFFFFF:
            raise ControlError("expected selected entity ID is outside the numeric ID range")
        expect["selected"] = args.expected_selected_entity_id
        lines.append(f"MeshAsset {expect['mesh']}")
        lines.append(f"ExpectedSelectedEntityId {args.expected_selected_entity_id}")
    elif command == "set-entity-mesh-renderer-assets":
        if args.entity_id <= 0 or args.entity_id > 0xFFFFFFFF:
            raise ControlError("entity ID must be a positive numeric ID")
        expect["entity"] = args.entity_id
        expect["name"] = args.expected_name
        expect["before"] = [_handle(args.expected_mesh_asset, "expected mesh asset", True),
                            _handle(args.expected_material_asset, "expected material asset", True)]
        expect["after"] = [_handle(args.new_mesh_asset, "new mesh asset"),
                           _handle(args.new_material_asset, "new material asset")]
        lines.append(f"EntityId {args.entity_id}")
        lines.append(f"ExpectedEntityName {_quote(args.expected_name)}")
        lines.append(f"ExpectedMeshAsset {expect['before'][0]}")
        lines.append(f"ExpectedMaterialAsset {expect['before'][1]}")
        lines.append(f"NewMeshAsset {expect['after'][0]}")
        lines.append(f"NewMaterialAsset {expect['after'][1]}")
    elif command == "save-project-state":
        if args.expected_manifest_sha256 is not None:
            if not _is_hex64(args.expected_manifest_sha256):
                raise ControlError("expected manifest SHA-256 must be 64 lowercase hex digits")
            expect["manifest"] = args.expected_manifest_sha256
            lines.append(f"ExpectedManifestSha256 {args.expected_manifest_sha256}")
    elif command == "set-fab-panel-visible":
        expect["visible"] = args.visible == "yes"
        lines.append(f"PanelVisible {args.visible}")
    return lines, expect


def _validate_fab_success(command: str, receipt: dict[str, object],
                          expect: dict[str, object]) -> None:
    fab = receipt["fab"]
    undo_unchanged = (receipt["undoDepthAfter"] == receipt["undoDepthBefore"]
                      and receipt["redoDepthAfter"] == receipt["redoDepthBefore"])
    one_history = (receipt["undoDepthAfter"] == min(receipt["undoDepthBefore"] + 1, 128)
                   and receipt["redoDepthAfter"] == 0)
    common = (receipt["action"] == FAB_COMMANDS[command] and receipt["reason"] == "ok"
              and receipt["postconditionVerified"] and not receipt["rollbackVerified"]
              and receipt["rendererGeneration"] > 0)
    session_only = receipt["persistence"] == "SessionOnly" and not receipt["saved"]
    if command == "place-mesh-asset":
        ok = (common and session_only and one_history and receipt["recovery"] == "UndoRedo"
              and receipt["effect"] == "MeshAssetPlaced" and receipt["entityId"] > 0
              and not receipt["beforeMeshRendererPresent"]
              and receipt["afterMeshRendererPresent"]
              and receipt["afterMeshRenderer"][0] == expect["mesh"]
              and receipt["selectedEntityIdBefore"] == expect["selected"]
              and receipt["selectedEntityIdAfter"] == receipt["entityId"]
              and receipt["selectionCommitted"]
              and receipt["affectedEntityIds"] == [receipt["entityId"]])
    elif command == "set-entity-mesh-renderer-assets":
        ok = (common and session_only and one_history and receipt["recovery"] == "UndoRedo"
              and receipt["effect"] == "MeshRendererAssetsSet"
              and receipt["entityId"] == expect["entity"]
              and receipt["entityName"] == expect["name"]
              and receipt["beforeMeshRendererPresent"] and receipt["afterMeshRendererPresent"]
              and receipt["beforeMeshRenderer"][:2] == expect["before"]
              and receipt["afterMeshRenderer"][:2] == expect["after"]
              and receipt["beforeMeshRenderer"][2:] == receipt["afterMeshRenderer"][2:])
    else:
        ok = (common and receipt["entityId"] == 0 and receipt["entityName"] == ""
              and receipt["affectedEntityCount"] == 0 and receipt["affectedEntityIds"] == []
              and not receipt["selectionCommitted"] and not receipt["pivotRetargeted"]
              and receipt["selectedEntityIdAfter"] == receipt["selectedEntityIdBefore"])
        if command == "commit-fab-import" and fab["projectChanged"]:
            ok = ok and receipt["undoDepthAfter"] == 0 and receipt["redoDepthAfter"] == 0
        else:
            ok = ok and undo_unchanged
        if command in ("inspect-fab", "wait-fab-state", "inspect-fab-panel"):
            ok = (ok and receipt["effect"] == "ReadOnly" and receipt["recovery"] == "None"
                  and session_only)
            if command != "inspect-fab-panel" and "job" in expect:
                ok = ok and fab["jobId"] == expect["job"]
        elif command == "select-fab-package":
            ok = (ok and receipt["effect"] == "FabPackageSelected" and session_only
                  and receipt["recovery"] == "CancelFabImport"
                  and fab["jobId"] > 0 and fab["sourceKind"] == expect["kind"]
                  and fab["sourceOrigin"] == "typed"
                  and fab["state"] in ("Snapshotting", "Preparing", "AwaitingProvenance"))
        elif command == "set-fab-provenance":
            ok = (ok and receipt["effect"] == "FabProvenanceSet" and session_only
                  and fab["state"] == "AwaitingProvenance" and fab["jobId"] == expect["job"]
                  and not fab["provenanceConfirmed"]
                  and fab["provenanceDigest"] == expect["digest"])
        elif command == "confirm-fab-provenance":
            ok = (ok and receipt["effect"] == "FabProvenanceConfirmed" and session_only
                  and fab["state"] in ("Cooking", "ReadyToCommit")
                  and fab["jobId"] == expect["job"] and fab["provenanceConfirmed"])
        elif command == "commit-fab-import":
            changed = fab["projectChanged"]
            ok = (ok and receipt["effect"] in ("FabImportCommitted", "FabImportReused")
                  and fab["state"] == "Done" and fab["jobId"] == expect["job"]
                  and fab["generationId"] == expect["generation"]
                  and fab["relation"] == expect["relation"]
                  and fab["meshAsset"] > 0 and fab["materialAsset"] > 0
                  and fab["resultHandleCount"] >= 2
                  and fab["assignmentApplied"] == bool(expect.get("assignment"))
                  and (receipt["effect"] == "FabImportCommitted") == changed
                  and receipt["persistence"] == ("Committed" if changed else "SessionOnly")
                  and receipt["saved"] == changed
                  and (not changed or (fab["manifestRevision"] >= 1
                                       and bool(fab["manifestSha256"]))))
        elif command == "cancel-fab-import":
            ok = (ok and receipt["effect"] == "FabCancelRequested" and session_only
                  and fab["jobId"] == expect["job"]
                  and (fab["cancelRequested"] or fab["state"] == "Cancelled"))
        elif command == "dismiss-fab-import":
            ok = (ok and receipt["effect"] == "FabImportDismissed" and session_only
                  and fab["state"] == "Idle")
        elif command == "save-project-state":
            ok = (ok and receipt["effect"] == "ProjectSaved" and receipt["persistence"] == "Saved"
                  and receipt["saved"] and receipt["recovery"] == "None"
                  and bool(fab["manifestSha256"]) and fab["projectStructural"] != "failed")
        elif command == "validate-project":
            ok = (ok and receipt["effect"] == "ProjectValidationStarted" and session_only
                  and fab["projectValidation"] == "running")
        elif command == "set-fab-panel-visible":
            ok = (ok and receipt["effect"] == "FabPanelVisibilitySet" and session_only
                  and fab["panel"]["visible"] == expect["visible"])
        else:
            ok = False
    if not ok:
        raise ControlError("successful receipt failed action-specific semantic validation")


def _run_fab_command(args, control_dir: Path, session: dict[str, object],
                     request_id: str) -> int:
    command = args.command
    action = FAB_COMMANDS[command]
    project_path = str(session["project_path"])
    session_id = str(session["session_id"])

    def header(identifier: str) -> list[str]:
        return [
            REQUEST_HEADER,
            f"RequestId {_quote(identifier, 'request ID', 64)}",
            f"SessionId {_quote(session_id, 'session ID', 128)}",
            f"ProjectPath {_quote(project_path, 'project path', 4096)}",
            f"Action {action}",
        ]

    if command == "wait-fab-state":
        if args.state is None and args.validation is None:
            raise ControlError("wait-fab-state needs --state and/or --validation")
        if not math.isfinite(args.timeout) or args.timeout <= 0.0:
            raise ControlError("--timeout must be finite and positive")
        expect: dict[str, object] = {}
        if args.expected_job_id is not None:
            expect["job"] = _job_id(args.expected_job_id)
        deadline = time.monotonic() + args.timeout
        delay = 0.05
        index = 0
        while True:
            identifier = _stable_id(f"{request_id}-p{index:03d}")
            index += 1
            lines = header(identifier)
            if "job" in expect:
                lines.append(f"ExpectedFabJobId {expect['job']}")
            receipt = _submit_request(control_dir, session, identifier, action, lines,
                                      args.timeout_seconds)
            receipt["editorProcessId"] = session["process_id"]
            if receipt["status"] != "Succeeded":
                print(json.dumps(receipt, separators=(",", ":"), sort_keys=True))
                return 2
            _validate_fab_success(command, receipt, expect)
            fab = receipt["fab"]
            reached = ((args.state is not None and fab["state"] in args.state)
                       or (args.validation is not None
                           and fab["projectValidation"] in args.validation))
            if reached:
                receipt["polls"] = index
                print(json.dumps(receipt, separators=(",", ":"), sort_keys=True))
                return 0
            if time.monotonic() >= deadline:
                raise ControlError(
                    f"timed out after {index} polls waiting for state={args.state} "
                    f"validation={args.validation}; last state={fab['state']} "
                    f"validation={fab['projectValidation']}")
            time.sleep(delay)
            delay = min(delay * 1.6, 0.4)

    request_lines, expect = _build_fab_request(args)
    receipt = _submit_request(control_dir, session, request_id, action,
                              header(request_id) + request_lines, args.timeout_seconds)
    if receipt["status"] == "Succeeded":
        _validate_fab_success(command, receipt, expect)
    receipt["editorProcessId"] = session["process_id"]
    print(json.dumps(receipt, separators=(",", ":"), sort_keys=True))
    return 0 if receipt["status"] == "Succeeded" else 2



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
    if args.command in FAB_COMMANDS:
        return _run_fab_command(args, control_dir, session, request_id)
    entity_actions = {"inspect", "set", "inspect-entity", "select-entity",
                      "set-transform", "set-viewport-main-camera-pose", "set-typed-light",
                      "set-mesh-renderer-flags"}
    if args.command in entity_actions and (args.entity_id <= 0 or args.entity_id > 0xFFFFFFFF):
        raise ControlError("entity ID must be a positive numeric ID")
    if args.command in ("inspect", "set") and (args.material_handle <= 0 or args.material_handle > 0xFFFFFFFFFFFFFFFF):
        raise ControlError("material handle must be a positive numeric ID")
    if (args.command in ("select-entity", "set-scene-debug-visualization")
            and not 0 <= args.expected_selected_entity_id <= 0xFFFFFFFF):
        raise ControlError("expected selected entity ID is outside the numeric ID range")
    action_names = {"inspect": "InspectMaterialSurface", "set": "SelectEntityPatchMaterialSurface",
                    "inspect-entity": "InspectEntity", "select-entity": "SelectEntity",
                    "set-transform": "SetEntityTransform", "set-typed-light": "SetTypedLight",
                    "set-project-color-pipeline": "SetProjectColorPipeline",
                    "set-viewport-main-camera-pose": "SetViewportMainCameraPose",
                    "set-scene-debug-visualization": "SetSceneDebugVisualization",
                    "set-mesh-renderer-flags": "SetMeshRendererFlags"}
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
    expected_debug_visualization: list[object] | None = None
    new_debug_visualization: list[object] | None = None
    expected_mesh_flags: list[bool] | None = None
    new_mesh_flags: list[bool] | None = None
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
    if args.command == "set-scene-debug-visualization":
        expected_bounds = args.expected_selected_bounds == "yes"
        new_bounds = args.new_selected_bounds == "yes"
        expected_debug_visualization = [args.expected_view, expected_bounds]
        new_debug_visualization = [args.new_view, new_bounds]
        lines.extend([
            f"ExpectedSelectedEntityId {args.expected_selected_entity_id}",
            f"ExpectedDebugVisualization {args.expected_view} {args.expected_selected_bounds}",
            f"NewDebugVisualization {args.new_view} {args.new_selected_bounds}",
        ])
    if args.command == "set-mesh-renderer-flags":
        expected_mesh_flags = [args.expected_visible == "yes",
                               args.expected_casts_shadows == "yes"]
        new_mesh_flags = [args.new_visible == "yes",
                          args.new_casts_shadows == "yes"]
        lines.extend([
            f"ExpectedMeshRendererFlags {args.expected_visible} {args.expected_casts_shadows}",
            f"NewMeshRendererFlags {args.new_visible} {args.new_casts_shadows}",
        ])
    receipt = _submit_request(control_dir, session, request_id, action, lines,
                              args.timeout_seconds)
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
            and receipt["persistence"] == "SessionOnly" and not receipt["saved"]
            and receipt["postconditionVerified"] and not receipt["rollbackVerified"])
        if args.command != "set-scene-debug-visualization":
            common_valid = (common_valid
                and receipt["beforeDebugVisualization"]
                    == receipt["afterDebugVisualization"])
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
        elif args.command == "set-mesh-renderer-flags":
            semantic_valid = (receipt["effect"] == "MeshRendererFlagsSet"
                and receipt["recovery"] == "UndoRedo" and one_history_entry
                and receipt["beforeMeshRendererPresent"]
                and receipt["afterMeshRendererPresent"]
                and receipt["beforeMeshRenderer"][:3]
                    == receipt["afterMeshRenderer"][:3]
                and receipt["beforeMeshRenderer"][3:] == expected_mesh_flags
                and receipt["afterMeshRenderer"][3:] == new_mesh_flags
                and transform_unchanged and camera_unchanged and light_unchanged
                and color_unchanged and not receipt["selectionCommitted"]
                and not receipt["pivotRetargeted"]
                and receipt["selectedEntityIdAfter"] == receipt["selectedEntityIdBefore"]
                and not receipt["rendererReadbackVerified"]
                and not receipt["editorCameraSynchronized"])
        elif args.command == "set-project-color-pipeline":
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
        else:
            semantic_valid = (receipt["entityId"] == 0 and receipt["entityName"] == ""
                and receipt["affectedEntityCount"] == 0
                and receipt["affectedEntityIds"] == []
                and receipt["effect"] == "SceneDebugVisualizationSet"
                and receipt["recovery"] == "RestorePreviousDebugVisualization"
                and history_unchanged
                and receipt["beforeDebugVisualization"] == expected_debug_visualization
                and receipt["afterDebugVisualization"] == new_debug_visualization
                and receipt["debugVisualizationGeneration"] > 0
                and receipt["selectedEntityIdBefore"] == args.expected_selected_entity_id
                and receipt["selectedEntityIdAfter"] == args.expected_selected_entity_id
                and transform_unchanged and camera_unchanged and light_unchanged
                and mesh_unchanged and color_unchanged
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
