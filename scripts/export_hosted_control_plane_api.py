#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
CONTRACT_DIR = ROOT / "contracts/hosted-control-plane/v1"
CONTRACT_PATH = CONTRACT_DIR / "hosted-control-plane.contract.json"
EXAMPLES_PATH = CONTRACT_DIR / "hosted-control-plane.examples.json"
OPENAPI_PATH = CONTRACT_DIR / "openapi.json"
ARAZZO_PATH = CONTRACT_DIR / "workflows.arazzo.json"

TAG_METADATA = {
    "workspaces": {
        "description": "Bootstrap and refresh hosted workspace state for frontend clients.",
    },
    "toolchain": {
        "description": "Install, select, pin, and clear managed compiler state.",
    },
    "dependencies": {
        "description": "Materialize and vendor dependency sources for a hosted workspace.",
    },
    "execution": {
        "description": "Run, build, and test the active workspace document through the hosted toolchain.",
    },
    "deployment": {
        "description": "Pack, preflight, and publish project artifacts from a hosted workspace.",
    },
}

OPERATION_METADATA = {
    "openWorkspace": {
        "tag": "workspaces",
        "summary": "Open or resume a hosted workspace",
        "description": (
            "Create or resume a hosted workspace session and return the initial project graph "
            "snapshot plus the hosted workspace record."
        ),
        "request_description": "Workspace bootstrap request consumed by frontend and control-console clients.",
    },
    "projectGraph": {
        "tag": "workspaces",
        "summary": "Refresh the hosted project graph",
        "description": (
            "Return the latest hosted project graph and workspace record for an existing hosted workspace."
        ),
        "request_description": "",
    },
    "toolInstall": {
        "tag": "toolchain",
        "summary": "Install a managed compiler",
        "description": "Install a managed Styio compiler into the hosted workspace toolchain surface.",
        "request_description": "Managed compiler installation request.",
    },
    "toolUse": {
        "tag": "toolchain",
        "summary": "Select the active managed compiler",
        "description": "Switch the hosted workspace to a specific managed compiler version and optional channel.",
        "request_description": "Managed compiler selection request.",
    },
    "toolPin": {
        "tag": "toolchain",
        "summary": "Pin the project compiler version",
        "description": "Persist a project-local compiler pin for the hosted workspace manifest.",
        "request_description": "Project toolchain pin request.",
    },
    "toolClearPin": {
        "tag": "toolchain",
        "summary": "Clear the project compiler pin",
        "description": "Remove the project-local compiler pin for the hosted workspace manifest.",
        "request_description": "Empty object request used to clear an existing project toolchain pin.",
    },
    "fetchDependencies": {
        "tag": "dependencies",
        "summary": "Fetch dependency sources",
        "description": "Resolve and materialize dependency sources into the hosted workspace cache.",
        "request_description": "Dependency fetch request.",
    },
    "vendorDependencies": {
        "tag": "dependencies",
        "summary": "Vendor dependency sources",
        "description": "Copy dependency sources into a vendor output path inside the hosted workspace.",
        "request_description": "Dependency vendor request.",
    },
    "runWorkflow": {
        "tag": "execution",
        "summary": "Run the active document workflow",
        "description": "Execute the active hosted document as a run workflow and return structured diagnostics and runtime events.",
        "request_description": "Hosted run-workflow request.",
    },
    "buildWorkflow": {
        "tag": "execution",
        "summary": "Build the active document workflow",
        "description": "Execute the active hosted document as a build workflow and return structured diagnostics and runtime events.",
        "request_description": "Hosted build-workflow request.",
    },
    "testWorkflow": {
        "tag": "execution",
        "summary": "Test the active document workflow",
        "description": "Execute the active hosted document as a test workflow and return structured diagnostics and runtime events.",
        "request_description": "Hosted test-workflow request.",
    },
    "packProject": {
        "tag": "deployment",
        "summary": "Pack the hosted project",
        "description": "Build a source package archive for the selected hosted package or workspace project.",
        "request_description": "Deployment pack request.",
    },
    "preparePublish": {
        "tag": "deployment",
        "summary": "Prepare publish preflight",
        "description": "Produce the publish-preflight result for a hosted package and preserve artifact paths for frontend flows.",
        "request_description": "Deployment preflight request.",
    },
    "publishToRegistry": {
        "tag": "deployment",
        "summary": "Publish to a registry",
        "description": "Publish a hosted package artifact to the selected registry root.",
        "request_description": "Registry publish request.",
    },
}

LINK_COMPONENTS = {
    "RefreshProjectGraph": {
        "operationId": "projectGraph",
        "parameters": {
            "workspace_id": "$response.body#/workspace/workspaceId",
        },
        "description": (
            "Refresh the project graph using the hosted workspace id returned by the bootstrap call."
        ),
    },
    "ActivateInstalledCompiler": {
        "operationId": "toolUse",
        "parameters": {
            "workspace_id": "$request.path.workspace_id",
        },
        "requestBody": {
            "compiler_version": "$response.body#/payload/compiler_version",
            "channel": "$response.body#/payload/channel",
        },
        "description": (
            "Activate the managed compiler that was just installed for the same hosted workspace."
        ),
    },
    "PinActivatedCompiler": {
        "operationId": "toolPin",
        "parameters": {
            "workspace_id": "$request.path.workspace_id",
        },
        "requestBody": {
            "compiler_version": "$response.body#/payload/compiler_version",
            "channel": "$response.body#/payload/channel",
        },
        "description": (
            "Persist the compiler version that was activated for the hosted workspace."
        ),
    },
    "PreparePackedProjectForPublish": {
        "operationId": "preparePublish",
        "parameters": {
            "workspace_id": "$request.path.workspace_id",
        },
        "requestBody": "$request.body",
        "description": (
            "Run publish preflight with the same package and output path used for pack."
        ),
    },
}

OPERATION_RESPONSE_LINKS = {
    "openWorkspace": {
        "refreshProjectGraph": "RefreshProjectGraph",
    },
    "toolInstall": {
        "activateInstalledCompiler": "ActivateInstalledCompiler",
    },
    "toolUse": {
        "pinActivatedCompiler": "PinActivatedCompiler",
    },
    "packProject": {
        "preparePackedProjectForPublish": "PreparePackedProjectForPublish",
    },
}


def load_json(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def dump_json(value: Any) -> str:
    return json.dumps(value, indent=2, ensure_ascii=False) + "\n"


def schema_from_spec(spec: dict[str, Any]) -> dict[str, Any]:
    if "ref" in spec:
        return {"$ref": f"#/components/schemas/{spec['ref']}"}

    spec_type = spec["type"]
    if spec_type == "string":
        schema: dict[str, Any] = {"type": "string"}
        if "min_length" in spec:
            schema["minLength"] = int(spec["min_length"])
        if "format" in spec:
            schema["format"] = spec["format"]
        return schema
    if spec_type == "integer":
        return {"type": "integer"}
    if spec_type == "boolean":
        return {"type": "boolean"}
    if spec_type == "enum":
        return {"type": "string", "enum": spec["values"]}
    if spec_type == "array":
        return {"type": "array", "items": schema_from_spec(spec["items"])}
    if spec_type == "map":
        return {
            "type": "object",
            "additionalProperties": schema_from_spec(spec["values"]),
        }
    if spec_type == "object":
        return {"type": "object"}
    raise ValueError(f"unsupported spec type: {spec_type}")


def schema_from_shape(shape_name: str, shape: dict[str, Any]) -> dict[str, Any]:
    kind = shape["kind"]
    if kind == "none":
        raise ValueError(f"shape {shape_name} has no request body")
    if kind != "object":
        raise ValueError(f"unsupported shape kind: {kind}")

    properties: dict[str, Any] = {}
    required = list(shape.get("required", {}).keys())

    for field_name, field_spec in shape.get("required", {}).items():
        properties[field_name] = schema_from_spec(field_spec)
    for field_name, field_spec in shape.get("optional", {}).items():
        properties[field_name] = schema_from_spec(field_spec)

    schema: dict[str, Any] = {
        "type": "object",
        "title": shape_name,
        "additionalProperties": False,
        "properties": properties,
    }
    if required:
        schema["required"] = required
    return schema


def build_schemas(contract: dict[str, Any]) -> dict[str, Any]:
    schemas: dict[str, Any] = {}
    for shape_name, shape in contract["shapes"].items():
        if shape.get("kind") == "none":
            continue
        schemas[shape_name] = schema_from_shape(shape_name, shape)
    return schemas


def build_examples(examples: dict[str, Any]) -> dict[str, Any]:
    rendered: dict[str, Any] = {}
    for operation_id, pack in examples.items():
        for variant in ("request", "success", "failure"):
            value = pack.get(variant)
            if value is None and variant == "request":
                continue
            rendered[f"{operation_id}_{variant}"] = {
                "summary": f"{operation_id} {variant}",
                "value": value,
            }
    return rendered


def build_request_body(operation: dict[str, Any], examples: dict[str, Any]) -> dict[str, Any] | None:
    request_shape = operation["request_shape"]
    if request_shape == "NoRequestBody":
        return None

    operation_id = operation["id"]
    request_examples = {
        "request": {"$ref": f"#/components/examples/{operation_id}_request"}
    }
    return {
        "description": OPERATION_METADATA[operation_id]["request_description"],
        "required": True,
        "content": {
            "application/json": {
                "schema": {"$ref": f"#/components/schemas/{request_shape}"},
                "examples": request_examples,
            }
        },
    }


def build_response(operation: dict[str, Any]) -> dict[str, Any]:
    operation_id = operation["id"]
    success_shape = operation["success_shape"]
    failure_shape = operation["failure_shape"]
    response_schema: dict[str, Any]
    if success_shape == failure_shape:
        response_schema = {"$ref": f"#/components/schemas/{success_shape}"}
    else:
        response_schema = {
            "oneOf": [
                {"$ref": f"#/components/schemas/{success_shape}"},
                {"$ref": f"#/components/schemas/{failure_shape}"},
            ]
        }

    response: dict[str, Any] = {
        "description": (
            "HTTP 200 returns either the success envelope or the documented domain-level failure "
            "envelope. Clients must inspect `returncode` and the payload family instead of "
            "guessing from frontend route context."
        ),
        "content": {
            "application/json": {
                "schema": response_schema,
                "examples": {
                    "success": {"$ref": f"#/components/examples/{operation_id}_success"},
                    "failure": {"$ref": f"#/components/examples/{operation_id}_failure"},
                },
            }
        },
    }
    response_links = OPERATION_RESPONSE_LINKS.get(operation_id)
    if response_links:
        response["links"] = {
            link_name: {"$ref": f"#/components/links/{component_name}"}
            for link_name, component_name in response_links.items()
        }
    return response


def workspace_path_parameter() -> dict[str, Any]:
    return {
        "name": "workspace_id",
        "in": "path",
        "required": True,
        "description": "Hosted workspace identifier returned by `openWorkspace`.",
        "schema": {
            "type": "string",
            "minLength": 1,
        },
    }


def build_operation(operation: dict[str, Any]) -> dict[str, Any]:
    operation_id = operation["id"]
    metadata = OPERATION_METADATA[operation_id]
    path_item: dict[str, Any] = {
        "tags": [metadata["tag"]],
        "summary": metadata["summary"],
        "description": metadata["description"],
        "operationId": operation_id,
        "responses": {
            "200": build_response(operation),
        },
    }
    if "{workspace_id}" in operation["path"]:
        path_item["parameters"] = [{"$ref": "#/components/parameters/workspace_id"}]
    request_body = build_request_body(operation, {})
    if request_body is not None:
        path_item["requestBody"] = request_body
    return path_item


def build_paths(contract: dict[str, Any]) -> dict[str, Any]:
    paths: dict[str, Any] = {}
    for operation in contract["operations"]:
        paths.setdefault(operation["path"], {})
        paths[operation["path"]][operation["method"].lower()] = build_operation(operation)
    return paths


def build_openapi(contract: dict[str, Any], examples: dict[str, Any]) -> dict[str, Any]:
    return {
        "openapi": "3.1.0",
        "jsonSchemaDialect": "https://spec.openapis.org/oas/3.1/dialect/base",
        "info": {
            "title": "spio hosted control-plane API",
            "version": "1.0.0",
            "summary": "Versioned hosted workspace API for styio-view and repo-hosted control-console clients.",
            "description": (
                "Design-first OpenAPI package for the hosted workspace API. The path family is frozen at "
                "`/api/styio-hosted/v1`; clients and backend services may vary base hostnames by deployment, "
                "but must preserve operation ids, request envelopes, response envelopes, and example payloads."
            ),
        },
        "servers": [
            {
                "url": "{scheme}://{host}/api/styio-hosted/v1",
                "description": "Deployment-neutral hosted control-plane base URL.",
                "variables": {
                    "scheme": {
                        "default": "https",
                        "enum": ["http", "https"],
                    },
                    "host": {
                        "default": "localhost:3000",
                    },
                },
            }
        ],
        "externalDocs": {
            "description": "Normative governance note for this API package.",
            "url": "https://github.com/eBioRing/styio-spio/blob/main/docs/governance/Spio-Hosted-Control-Plane-Contract.md",
        },
        "tags": [
            {"name": tag_name, **tag_body}
            for tag_name, tag_body in TAG_METADATA.items()
        ],
        "paths": build_paths(contract),
        "components": {
            "parameters": {
                "workspace_id": workspace_path_parameter(),
            },
            "schemas": build_schemas(contract),
            "examples": build_examples(examples),
            "links": LINK_COMPONENTS,
        },
    }


def build_arazzo() -> dict[str, Any]:
    return {
        "arazzo": "1.0.1",
        "info": {
            "title": "spio hosted control-plane workflows",
            "version": "1.0.0",
            "summary": "Frontend/backend workflow descriptions built on top of the hosted control-plane OpenAPI package.",
            "description": (
                "These workflows show how frontend and backend teams coordinate across the frozen "
                "OpenAPI operation set without depending on private source layout."
            ),
        },
        "sourceDescriptions": [
            {
                "name": "hostedControlPlane",
                "url": "./openapi.json",
                "type": "openapi",
            }
        ],
        "workflows": [
            {
                "workflowId": "bootstrapWorkspaceAndProjectGraph",
                "summary": "Open a hosted workspace and load the initial project graph.",
                "description": "Bootstrap flow used by user-facing frontend shells and repo-hosted control-console pages.",
                "inputs": {
                    "type": "object",
                    "properties": {
                        "workspace_root": {"type": "string"},
                        "manifest_path": {"type": "string"},
                        "platform": {"type": "string"},
                    },
                    "required": ["workspace_root", "platform"],
                },
                "steps": [
                    {
                        "stepId": "openWorkspace",
                        "description": "Create or resume a hosted workspace session.",
                        "operationId": "openWorkspace",
                        "requestBody": {
                            "contentType": "application/json",
                            "payload": {
                                "workspace_root": "$inputs.workspace_root",
                                "manifest_path": "$inputs.manifest_path",
                                "platform": "$inputs.platform",
                            },
                        },
                        "successCriteria": [
                            {"condition": "$statusCode == 200"},
                            {"condition": "$response.body.returncode == 0"},
                        ],
                        "outputs": {
                            "workspaceId": "$response.body#/workspace/workspaceId",
                        },
                    },
                    {
                        "stepId": "projectGraph",
                        "description": "Refresh the project graph with the workspace id returned by the bootstrap step.",
                        "operationId": "projectGraph",
                        "parameters": [
                            {
                                "name": "workspace_id",
                                "in": "path",
                                "value": "$steps.openWorkspace.outputs.workspaceId",
                            }
                        ],
                        "successCriteria": [
                            {"condition": "$statusCode == 200"},
                            {"condition": "$response.body.returncode == 0"},
                        ],
                    },
                ],
                "outputs": {
                    "workspace_id": "$steps.openWorkspace.outputs.workspaceId",
                },
            },
            {
                "workflowId": "installUseAndPinManagedCompiler",
                "summary": "Install, activate, and pin a managed compiler for a hosted workspace.",
                "description": "Toolchain-management flow used by frontend environment panels and control-console tooling.",
                "inputs": {
                    "type": "object",
                    "properties": {
                        "workspace_id": {"type": "string"},
                        "styio_binary_path": {"type": "string"},
                        "compiler_version": {"type": "string"},
                        "channel": {"type": "string"},
                    },
                    "required": ["workspace_id", "styio_binary_path", "compiler_version"],
                },
                "steps": [
                    {
                        "stepId": "toolInstall",
                        "description": "Install the managed compiler into the hosted workspace toolchain surface.",
                        "operationId": "toolInstall",
                        "parameters": [
                            {
                                "name": "workspace_id",
                                "in": "path",
                                "value": "$inputs.workspace_id",
                            }
                        ],
                        "requestBody": {
                            "contentType": "application/json",
                            "payload": {
                                "styio_binary_path": "$inputs.styio_binary_path",
                            },
                        },
                        "successCriteria": [
                            {"condition": "$statusCode == 200"},
                            {"condition": "$response.body.returncode == 0"},
                        ],
                    },
                    {
                        "stepId": "toolUse",
                        "description": "Switch the active compiler version for the hosted workspace.",
                        "operationId": "toolUse",
                        "parameters": [
                            {
                                "name": "workspace_id",
                                "in": "path",
                                "value": "$inputs.workspace_id",
                            }
                        ],
                        "requestBody": {
                            "contentType": "application/json",
                            "payload": {
                                "compiler_version": "$inputs.compiler_version",
                                "channel": "$inputs.channel",
                            },
                        },
                        "successCriteria": [
                            {"condition": "$statusCode == 200"},
                            {"condition": "$response.body.returncode == 0"},
                        ],
                    },
                    {
                        "stepId": "toolPin",
                        "description": "Persist the project-level compiler pin after selection succeeds.",
                        "operationId": "toolPin",
                        "parameters": [
                            {
                                "name": "workspace_id",
                                "in": "path",
                                "value": "$inputs.workspace_id",
                            }
                        ],
                        "requestBody": {
                            "contentType": "application/json",
                            "payload": {
                                "compiler_version": "$inputs.compiler_version",
                                "channel": "$inputs.channel",
                            },
                        },
                        "successCriteria": [
                            {"condition": "$statusCode == 200"},
                            {"condition": "$response.body.returncode == 0"},
                        ],
                    },
                ],
            },
            {
                "workflowId": "dependencyExecutionAndPackagingLoop",
                "summary": "Fetch dependencies, run the active document, and pack an artifact.",
                "description": "End-to-end user-facing workflow that spans dependency materialization, execution, and deployment-prep surfaces.",
                "inputs": {
                    "type": "object",
                    "properties": {
                        "workspace_id": {"type": "string"},
                        "active_file_path": {"type": "string"},
                        "document_text": {"type": "string"},
                        "package_name": {"type": "string"},
                        "target_name": {"type": "string"},
                        "target_kind": {"type": "string"},
                    },
                    "required": ["workspace_id", "active_file_path", "document_text"],
                },
                "steps": [
                    {
                        "stepId": "fetchDependencies",
                        "description": "Refresh dependency sources before execution or packaging.",
                        "operationId": "fetchDependencies",
                        "parameters": [
                            {
                                "name": "workspace_id",
                                "in": "path",
                                "value": "$inputs.workspace_id",
                            }
                        ],
                        "requestBody": {
                            "contentType": "application/json",
                            "payload": {
                                "locked": True,
                                "offline": False,
                            },
                        },
                        "successCriteria": [
                            {"condition": "$statusCode == 200"},
                            {"condition": "$response.body.returncode == 0"},
                        ],
                    },
                    {
                        "stepId": "runWorkflow",
                        "description": "Run the active document and capture structured runtime events.",
                        "operationId": "runWorkflow",
                        "parameters": [
                            {
                                "name": "workspace_id",
                                "in": "path",
                                "value": "$inputs.workspace_id",
                            }
                        ],
                        "requestBody": {
                            "contentType": "application/json",
                            "payload": {
                                "active_file_path": "$inputs.active_file_path",
                                "document_text": "$inputs.document_text",
                                "package_name": "$inputs.package_name",
                                "target_name": "$inputs.target_name",
                                "target_kind": "$inputs.target_kind",
                            },
                        },
                        "successCriteria": [
                            {"condition": "$statusCode == 200"},
                            {"condition": "$response.body.returncode == 0"},
                        ],
                    },
                    {
                        "stepId": "packProject",
                        "description": "Produce a distributable source package artifact.",
                        "operationId": "packProject",
                        "parameters": [
                            {
                                "name": "workspace_id",
                                "in": "path",
                                "value": "$inputs.workspace_id",
                            }
                        ],
                        "requestBody": {
                            "contentType": "application/json",
                            "payload": {
                                "package_name": "$inputs.package_name",
                            },
                        },
                        "successCriteria": [
                            {"condition": "$statusCode == 200"},
                            {"condition": "$response.body.returncode == 0"},
                        ],
                        "outputs": {
                            "archive_path": "$response.body#/payload/archive_path",
                        },
                    },
                ],
                "outputs": {
                    "archive_path": "$steps.packProject.outputs.archive_path",
                },
            },
            {
                "workflowId": "preflightAndPublishArtifact",
                "summary": "Prepare a publish candidate and publish it to a registry.",
                "description": "Deployment workflow for frontend release surfaces and control-console publish actions.",
                "inputs": {
                    "type": "object",
                    "properties": {
                        "workspace_id": {"type": "string"},
                        "registry_root": {"type": "string"},
                        "package_name": {"type": "string"},
                        "output_path": {"type": "string"},
                    },
                    "required": ["workspace_id", "registry_root"],
                },
                "steps": [
                    {
                        "stepId": "preparePublish",
                        "description": "Run publish preflight and preserve the generated artifact path.",
                        "operationId": "preparePublish",
                        "parameters": [
                            {
                                "name": "workspace_id",
                                "in": "path",
                                "value": "$inputs.workspace_id",
                            }
                        ],
                        "requestBody": {
                            "contentType": "application/json",
                            "payload": {
                                "package_name": "$inputs.package_name",
                                "output_path": "$inputs.output_path",
                            },
                        },
                        "successCriteria": [
                            {"condition": "$statusCode == 200"},
                            {"condition": "$response.body.returncode == 0"},
                        ],
                    },
                    {
                        "stepId": "publishToRegistry",
                        "description": "Publish the preflighted artifact to the selected registry root.",
                        "operationId": "publishToRegistry",
                        "parameters": [
                            {
                                "name": "workspace_id",
                                "in": "path",
                                "value": "$inputs.workspace_id",
                            }
                        ],
                        "requestBody": {
                            "contentType": "application/json",
                            "payload": {
                                "registry_root": "$inputs.registry_root",
                                "package_name": "$inputs.package_name",
                                "output_path": "$inputs.output_path",
                            },
                        },
                        "successCriteria": [
                            {"condition": "$statusCode == 200"},
                            {"condition": "$response.body.returncode == 0"},
                        ],
                    },
                ],
            },
        ],
    }


def write_outputs() -> None:
    contract = load_json(CONTRACT_PATH)
    examples = load_json(EXAMPLES_PATH)
    OPENAPI_PATH.write_text(dump_json(build_openapi(contract, examples)), encoding="utf-8")
    ARAZZO_PATH.write_text(dump_json(build_arazzo()), encoding="utf-8")


def check_outputs() -> bool:
    contract = load_json(CONTRACT_PATH)
    examples = load_json(EXAMPLES_PATH)
    expected_openapi = dump_json(build_openapi(contract, examples))
    expected_arazzo = dump_json(build_arazzo())
    current_openapi = OPENAPI_PATH.read_text(encoding="utf-8") if OPENAPI_PATH.exists() else ""
    current_arazzo = ARAZZO_PATH.read_text(encoding="utf-8") if ARAZZO_PATH.exists() else ""
    return current_openapi == expected_openapi and current_arazzo == expected_arazzo


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate or verify hosted control-plane OpenAPI and Arazzo artifacts.")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true", help="rewrite generated OpenAPI and Arazzo artifacts")
    group.add_argument("--check", action="store_true", help="verify generated artifacts are current")
    args = parser.parse_args()

    if args.write:
        write_outputs()
        print("hosted control-plane OpenAPI/Arazzo artifacts updated")
        return 0

    if check_outputs():
        print("hosted control-plane OpenAPI/Arazzo artifacts are current")
        return 0

    print("hosted control-plane OpenAPI/Arazzo artifacts are out of date", flush=True)
    print("Run: python3 scripts/export_hosted_control_plane_api.py --write", flush=True)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
