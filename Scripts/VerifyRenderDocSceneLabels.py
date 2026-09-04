#!/usr/bin/env python3
"""qrenderdoc --python entry point for the Vulkan scene-label capture check."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import time


SCHEMA = "SpiralRenderDocSceneLabelsV1"
SUPPORTED_RENDERDOC_VERSION = "1.45"
BASE_LABELS = [
    "Scene Light Payload Copy",
    "Scene Primary Directional Shadow Map",
    "Scene Viewport Graph Clear",
    "Scene Sky Atmosphere",
    "Scene Viewport Graph Raster",
    "Scene Viewport Graph Tone Map",
    "Scene Viewport Graph Output Handoff",
]
OVERLAY_LABELS = BASE_LABELS[:-1] + [
    "Scene Debug Overlay",
    BASE_LABELS[-1],
]


def required_environment(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        raise RuntimeError(f"missing required environment variable {name}")
    return value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def action_names(controller, structured_file) -> list[str]:
    names: list[str] = []

    def visit(actions) -> None:
        for action in actions:
            names.append(str(action.GetName(structured_file)))
            visit(action.children)

    visit(controller.GetRootActions())
    return names


def result_message(details) -> str:
    return str(details.Message())


def main() -> int:
    report_path = Path(required_environment("SPIRAL_RENDERDOC_REPORT"))
    capture_path = Path(required_environment("SPIRAL_RENDERDOC_CAPTURE"))
    capture_template = required_environment("SPIRAL_RENDERDOC_CAPTURE_TEMPLATE")
    qrenderdoc = Path(required_environment("SPIRAL_RENDERDOC_QRENDERDOC"))
    renderdoc_library = Path(required_environment("SPIRAL_RENDERDOC_LIBRARY"))
    source_layer_manifest = Path(required_environment(
        "SPIRAL_RENDERDOC_SOURCE_VULKAN_LAYER_MANIFEST"
    ))
    effective_layer_manifest = Path(required_environment(
        "SPIRAL_RENDERDOC_EFFECTIVE_VULKAN_LAYER_MANIFEST"
    ))
    vulkan_layer_directory = required_environment(
        "SPIRAL_RENDERDOC_VULKAN_LAYER_DIR"
    )
    runtime_library_path = required_environment(
        "SPIRAL_RENDERDOC_RUNTIME_LIBRARY_PATH"
    )
    editor = Path(required_environment("SPIRAL_RENDERDOC_EDITOR"))
    repository = Path(required_environment("SPIRAL_RENDERDOC_REPOSITORY"))
    timeout_seconds = int(required_environment("SPIRAL_RENDERDOC_TIMEOUT_SECONDS"))
    command_line = (
        "--renderer-vulkan --vulkan-render-smoke "
        "--scene-viewport-render-graph-smoke"
    )

    report: dict[str, object] = {
        "schema": SCHEMA,
        "result": "fail",
        "tool": {
            "qrenderdoc": str(qrenderdoc),
            "qrenderdocSha256": sha256_file(qrenderdoc),
            "renderdocLibrary": str(renderdoc_library),
            "renderdocLibrarySha256": sha256_file(renderdoc_library),
            "sourceVulkanLayerManifest": str(source_layer_manifest),
            "sourceVulkanLayerManifestSha256": sha256_file(
                source_layer_manifest
            ),
            "effectiveVulkanLayerManifest": str(effective_layer_manifest),
            "effectiveVulkanLayerManifestSha256": sha256_file(
                effective_layer_manifest
            ),
            "vulkanImplicitLayerPath": vulkan_layer_directory,
            "runtimeLibraryPath": runtime_library_path,
            "childEnvironment": {
                "VK_IMPLICIT_LAYER_PATH": vulkan_layer_directory,
                "ENABLE_VULKAN_RENDERDOC_CAPTURE": "1",
            },
            "removedFromQrenderdocEnvironment": [
                "ENABLE_VULKAN_RENDERDOC_CAPTURE",
                "DISABLE_VULKAN_RENDERDOC_CAPTURE_1_45",
                "VK_IMPLICIT_LAYER_PATH",
                "VK_LOADER_LAYERS_DISABLE",
            ],
        },
        "launch": {
            "editor": str(editor),
            "editorSha256": sha256_file(editor),
            "repository": str(repository),
            "commandLine": command_line,
            "timeoutSeconds": timeout_seconds,
        },
        "capture": {"path": str(capture_path)},
        "expectedVariants": [BASE_LABELS, OVERLAY_LABELS],
        "observedSceneLabels": [],
        "actionNameCount": 0,
        "actionNames": [],
        "targetControl": {
            "triggerMode": "immediate-next-presented-frame",
            "triggered": False,
            "registeredApis": [],
        },
        "error": "verification did not complete",
    }

    target = None
    capture_file = None
    controller = None
    source_capture_id: int | None = None
    try:
        import renderdoc as rd

        renderdoc_version = str(rd.GetVersionString())
        report["tool"].update({
            "version": renderdoc_version,
            "commit": str(rd.GetCommitHash()),
        })
        if renderdoc_version != SUPPORTED_RENDERDOC_VERSION:
            raise RuntimeError(
                "unsupported RenderDoc Python API version: "
                f"expected {SUPPORTED_RENDERDOC_VERSION}, got {renderdoc_version}"
            )

        child_environment = []
        for name, value in (
            ("VK_IMPLICIT_LAYER_PATH", vulkan_layer_directory),
            ("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1"),
        ):
            modification = rd.EnvironmentModification()
            modification.name = name
            modification.value = value
            modification.mod = rd.EnvMod.Set
            modification.sep = rd.EnvSep.Platform
            child_environment.append(modification)

        launch = rd.ExecuteAndInject(
            str(editor),
            str(repository),
            command_line,
            child_environment,
            capture_template,
            rd.CaptureOptions(),
            False,
        )
        if not launch.result.OK() or launch.ident == 0:
            raise RuntimeError(
                "RenderDoc ExecuteAndInject failed: "
                + result_message(launch.result)
            )

        target = rd.CreateTargetControl(
            "", launch.ident, "SpiralRenderDocSceneLabelsV1", False
        )
        if target is None:
            raise RuntimeError(
                f"RenderDoc could not connect to target ident {launch.ident}"
            )

        # Queue the capture as soon as target control connects. Waiting for a
        # RegisterAPI message races bounded smoke executables that can present
        # and exit before the controller processes that notification.
        target.TriggerCapture(1)
        triggered = True
        report["targetControl"]["triggered"] = True
        deadline = time.monotonic() + timeout_seconds
        source_capture = None
        while time.monotonic() < deadline:
            if not target.Connected():
                raise RuntimeError(
                    "RenderDoc target disconnected before a capture was received"
                )
            message = target.ReceiveMessage(None)
            if message.type == rd.TargetControlMessageType.RegisterAPI:
                api_name = str(message.apiUse.name)
                report["targetControl"]["registeredApis"].append({
                    "name": api_name,
                    "presenting": bool(message.apiUse.presenting),
                    "supported": bool(message.apiUse.supported),
                })
            elif message.type == rd.TargetControlMessageType.Busy:
                raise RuntimeError(
                    "RenderDoc target is controlled by another client: "
                    + str(message.busy.clientName)
                )
            elif message.type == rd.TargetControlMessageType.Disconnected:
                raise RuntimeError(
                    "RenderDoc target disconnected before a capture was received"
                )
            elif message.type == rd.TargetControlMessageType.NewCapture:
                if not triggered:
                    raise RuntimeError(
                        "RenderDoc reported an unrequested capture before the Vulkan trigger"
                    )
                source_capture = message.newCapture
                source_capture_id = int(source_capture.captureId)
                break

        if source_capture is None:
            raise RuntimeError("RenderDoc capture timed out after the immediate trigger")

        source_path = Path(str(source_capture.path))
        if not source_capture.local or not source_path.is_file():
            raise RuntimeError(
                "RenderDoc returned a non-local or missing capture; this verifier "
                "only accepts a local Linux target"
            )
        if int(source_capture.byteSize) <= 0:
            raise RuntimeError("RenderDoc returned an empty capture")

        source_api = str(source_capture.api)
        if "Vulkan" not in source_api:
            raise RuntimeError(
                f"RenderDoc captured the wrong graphics API: {source_api or '<empty>'}"
            )

        shutil.copy2(source_path, capture_path)
        if not capture_path.is_file() or capture_path.stat().st_size <= 0:
            raise RuntimeError("RenderDoc capture copy was not preserved")

        capture_file = rd.OpenCaptureFile()
        opened = capture_file.OpenFile(str(capture_path), "rdc", None)
        if not opened.OK():
            raise RuntimeError(
                "RenderDoc could not open the preserved capture: "
                + result_message(opened)
            )
        replayed, controller = capture_file.OpenCapture(rd.ReplayOptions(), None)
        if not replayed.OK() or controller is None:
            raise RuntimeError(
                "RenderDoc could not replay the preserved capture: "
                + result_message(replayed)
            )

        names = action_names(controller, capture_file.GetStructuredData())
        known_labels = set(OVERLAY_LABELS)
        observed = [name for name in names if name in known_labels]
        if observed not in (BASE_LABELS, OVERLAY_LABELS):
            raise RuntimeError(
                "captured scene labels were not the exact seven-pass base sequence "
                "or selected-bounds eight-pass sequence: "
                + json.dumps(observed)
            )

        report["capture"] = {
            "path": str(capture_path),
            "sha256": sha256_file(capture_path),
            "byteSize": capture_path.stat().st_size,
            "frameNumber": int(source_capture.frameNumber),
            "api": source_api,
        }
        report["observedSceneLabels"] = observed
        report["actionNameCount"] = len(names)
        report["actionNames"] = names
        report["result"] = "pass"
        report["error"] = ""
        return 0
    except Exception as error:  # qrenderdoc owns the interpreter and reports poorly by default.
        report["error"] = f"{type(error).__name__}: {error}"
        return 1
    finally:
        if controller is not None:
            try:
                controller.Shutdown()
            except Exception:
                pass
        if capture_file is not None:
            try:
                capture_file.Shutdown()
            except Exception:
                pass
        if target is not None:
            if source_capture_id is not None:
                try:
                    target.DeleteCapture(source_capture_id)
                except Exception:
                    pass
            try:
                target.Shutdown()
            except Exception:
                pass
        report_path.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        print(
            f"{SCHEMA} result={report['result']} report={report_path} "
            f"capture={capture_path} error={report['error']}",
            flush=True,
        )


raise SystemExit(main())
