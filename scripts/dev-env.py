#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import secrets
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

from styio_yaml import YamlConfigError, dump_yaml_mapping, load_config_section, load_yaml_mapping, write_yaml_mapping


ROOT = Path(__file__).resolve().parents[1]
CHART = ROOT / "deploy/helm/styio-platform"
STATE_ROOT = ROOT / ".styio-platform/dev-env"
DEFAULT_CONFIG = ROOT / "config/styio-platform-tools.yaml"
DEV_ENV_DEFAULTS: dict[str, object] = {
    "cluster": "styio-platform-dev",
    "namespace": "styio-platform-dev",
    "release": "styio",
    "image": "localhost/styio-platform:dev",
    "image_builder": "podman",
    "build_image": True,
    "port_forward": True,
    "primary_port": 8787,
    "mirror_port": 8080,
    "delete_cluster": False,
    "auto_register_workgroup": True,
    "workgroup_id": "styio-local-workgroup",
    "workgroup_cluster_id": "styio-platform-dev",
    "workgroup_trust_domain": "styio-platform-local",
    "workgroup_registration_policy": "local-dev-default",
    "workgroup_registration_tenant": "platform",
    "workgroup_registration_token": "styio-local-dev-token",
    "region": "local-dev",
}
IDENTITY_HEADER = {
    "X-Styio-Mtls-Uri-San": "spiffe://styio-platform/tenant/platform/role/operator/node/dev-env",
}


def run(
    cmd: list[str],
    *,
    check: bool = True,
    capture: bool = False,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, cwd=ROOT, text=True, check=check, capture_output=capture, env=env)


def kind_env() -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("KIND_EXPERIMENTAL_PROVIDER", "podman")
    return env


def run_kind(cmd: list[str], *, check: bool = True, capture: bool = False) -> subprocess.CompletedProcess[str]:
    return run(cmd, check=check, capture=capture, env=kind_env())


def require_tools(builder: str) -> None:
    required = ["kind", "kubectl", "helm", "podman"]
    if builder == "buildah":
        required.append("buildah")
    missing = [tool for tool in required if shutil.which(tool) is None]
    if missing:
        raise SystemExit(f"missing required open dev-env tools: {', '.join(missing)}")


def state_dir(cluster: str, namespace: str, release: str) -> Path:
    return STATE_ROOT / f"{cluster}-{namespace}-{release}"


def endpoint_state_path(state: Path) -> Path:
    return state / "endpoints.yaml"


def workgroup_state_path(state: Path) -> Path:
    return state / "workgroup.yaml"


def normalize_api_url(url: str) -> str:
    return url.rstrip("/")


def api_url(base_url: str, path: str) -> str:
    return normalize_api_url(base_url) + "/" + path.lstrip("/")


def load_recorded_endpoints(state: Path) -> dict[str, str]:
    endpoints_path = endpoint_state_path(state)
    if endpoints_path.exists():
        data = load_yaml_mapping(endpoints_path)
        return {str(key): str(value) for key, value in data.items() if isinstance(value, str)}
    legacy_path = state / "endpoints.json"
    if legacy_path.exists():
        data = json.loads(legacy_path.read_text(encoding="utf-8"))
        return {str(key): str(value) for key, value in data.items() if isinstance(value, str)}
    return {}


def writeable_yaml_preview(mapping: dict[str, str]) -> str:
    return dump_yaml_mapping(mapping)


def resolve_config_path(raw_path: str | None) -> Path:
    path = Path(raw_path) if raw_path else DEFAULT_CONFIG
    if not path.is_absolute():
        path = ROOT / path
    return path


def typed_config_value(path: Path, key: str, value: object, fallback: object) -> object:
    if isinstance(fallback, bool):
        if not isinstance(value, bool):
            raise YamlConfigError(f"{path}: dev_env.{key} must be a boolean")
        return value
    if isinstance(fallback, int) and not isinstance(fallback, bool):
        if not isinstance(value, int) or isinstance(value, bool):
            raise YamlConfigError(f"{path}: dev_env.{key} must be an integer")
        return value
    if isinstance(fallback, str):
        if not isinstance(value, str):
            raise YamlConfigError(f"{path}: dev_env.{key} must be a string")
        return value
    return value


def apply_tool_config(args: argparse.Namespace) -> None:
    config_path = resolve_config_path(args.config)
    required = config_path != DEFAULT_CONFIG
    config = load_config_section(config_path, "dev_env", required=required)
    for key, fallback in DEV_ENV_DEFAULTS.items():
        cli_value = getattr(args, key, None)
        if cli_value is not None:
            continue
        config_value = config.get(key, fallback)
        setattr(args, key, typed_config_value(config_path, key, config_value, fallback))
    if args.image_builder not in {"podman", "buildah"}:
        raise YamlConfigError(f"{config_path}: dev_env.image_builder must be podman or buildah")
    args.image = normalize_kind_image(args.image)


def has_explicit_registry(image: str) -> bool:
    first_component = image.split("/", 1)[0]
    return first_component == "localhost" or "." in first_component or ":" in first_component


def normalize_kind_image(image: str) -> str:
    if has_explicit_registry(image):
        return image
    return f"localhost/{image}"


def request_json(base_url: str, path: str, *, method: str = "GET", body: dict[str, object] | None = None) -> dict[str, object]:
    data = None
    headers = dict(IDENTITY_HEADER)
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    try:
        request = Request(api_url(base_url, path), data=data, headers=headers, method=method)
        with urlopen(request, timeout=10) as response:
            return json.loads(response.read().decode("utf-8"))
    except HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"{method} {api_url(base_url, path)} failed with HTTP {error.code}: {detail}") from error
    except URLError as error:
        raise RuntimeError(f"{method} {api_url(base_url, path)} failed: {error}") from error


def internal_primary_endpoint(namespace: str, release: str) -> str:
    return f"http://{release}-styio-platform-primary.{namespace}.svc.cluster.local:8787/api/styio-platform/v1"


def workgroup_cluster_payload(
    args: argparse.Namespace,
    endpoints: dict[str, str],
    *,
    cluster_id: str | None = None,
    primary_url: str | None = None,
    node_id: str | None = None,
    region: str | None = None,
) -> dict[str, object]:
    resolved_cluster_id = cluster_id or args.workgroup_cluster_id or args.cluster
    resolved_primary = primary_url or endpoints.get("primary", "")
    payload: dict[str, object] = {
        "cluster_id": resolved_cluster_id,
        "region": region or args.region,
        "node_id": node_id or f"{resolved_cluster_id}-primary",
        "control_plane_endpoint": normalize_api_url(resolved_primary),
        "internal_control_plane_endpoint": internal_primary_endpoint(args.namespace, args.release),
        "roles": ["control-plane", "worker", "mirror", "registry-writer"],
        "trust_domain": args.workgroup_trust_domain,
        "registration_token": args.workgroup_registration_token,
        "labels": {
            "source": "dev-env",
            "cluster": args.cluster,
            "namespace": args.namespace,
            "release": args.release,
        },
    }
    if endpoints.get("mirror_registry"):
        payload["registry_endpoint"] = normalize_api_url(endpoints["mirror_registry"])
    return payload


def register_cluster(base_url: str, workgroup_id: str, payload: dict[str, object]) -> dict[str, object]:
    return request_json(
        base_url,
        f"/workgroups/{workgroup_id}/clusters/register",
        method="POST",
        body=payload,
    )


def list_workgroup(base_url: str, workgroup_id: str) -> dict[str, object]:
    return request_json(base_url, f"/workgroups/{workgroup_id}/clusters")


def self_register_workgroup(args: argparse.Namespace, endpoints: dict[str, str]) -> dict[str, object] | None:
    primary = endpoints.get("primary", "")
    if not primary:
        return None
    payload = workgroup_cluster_payload(args, endpoints)
    return register_cluster(primary, args.workgroup_id, payload)


def port_available(port: int) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def reserve_port(preferred: int) -> int:
    if preferred > 0 and port_available(preferred):
        return preferred
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def build_oci_image(builder: str, image: str) -> Path:
    archive = Path(tempfile.mkdtemp(prefix="styio-platform-dev-image-")) / "image.tar"
    if builder == "podman":
        run(["podman", "build", "-f", "Containerfile", "-t", image, "."])
        run(["podman", "save", "-o", str(archive), image])
        return archive
    if builder == "buildah":
        run(["buildah", "bud", "-f", "Containerfile", "-t", image, "."])
        run(["buildah", "push", image, f"docker-archive:{archive}:{image}"])
        return archive
    raise AssertionError(f"unsupported image builder: {builder}")


def ensure_cluster(cluster: str) -> None:
    existing = run_kind(["kind", "get", "clusters"], check=False, capture=True)
    if cluster not in existing.stdout.splitlines():
        run_kind(["kind", "create", "cluster", "--name", cluster])


def load_image(cluster: str, image_archive: Path) -> None:
    run_kind(["kind", "load", "image-archive", str(image_archive), "--name", cluster])


def helm_install(args: argparse.Namespace, postgres_password: str) -> None:
    image_repo, image_tag = args.image.rsplit(":", 1)
    run(["kubectl", "create", "namespace", args.namespace], check=False)
    run(
        [
            "helm",
            "upgrade",
            "--install",
            args.release,
            str(CHART),
            "-n",
            args.namespace,
            "--set",
            f"image.repository={image_repo}",
            "--set",
            f"image.tag={image_tag}",
            "--set",
            "image.pullPolicy=Never",
            "--set",
            f"postgres.password={postgres_password}",
            "--set",
            f"workgroup.id={args.workgroup_id}",
            "--set",
            f"workgroup.trustDomain={args.workgroup_trust_domain}",
            "--set",
            f"workgroup.registrationPolicy={args.workgroup_registration_policy}",
            "--set",
            f"workgroup.registrationTenant={args.workgroup_registration_tenant}",
            "--set",
            f"workgroup.registrationToken={args.workgroup_registration_token}",
        ]
    )


def wait_rollouts(namespace: str, release: str) -> None:
    rollout_status(namespace, release, f"statefulset/{release}-styio-platform-postgres")
    rollout_status(namespace, release, f"deploy/{release}-styio-platform-primary")
    rollout_status(namespace, release, f"deploy/{release}-styio-platform-worker")
    rollout_status(namespace, release, f"deploy/{release}-styio-platform-mirror")


def dump_rollout_diagnostics(namespace: str, release: str) -> None:
    print("styio-platform dev rollout diagnostics", flush=True)
    diagnostic_commands = [
        ["kubectl", "-n", namespace, "get", "pods,deploy,statefulset,pvc,svc", "-o", "wide"],
        ["kubectl", "-n", namespace, "get", "events", "--sort-by=.lastTimestamp"],
        ["kubectl", "-n", namespace, "describe", "pods", "-l", f"app.kubernetes.io/instance={release}"],
        ["kubectl", "-n", namespace, "logs", f"deploy/{release}-styio-platform-primary", "--all-containers", "--tail=200", "--prefix"],
        ["kubectl", "-n", namespace, "logs", f"deploy/{release}-styio-platform-worker", "--all-containers", "--tail=200", "--prefix"],
        ["kubectl", "-n", namespace, "logs", f"deploy/{release}-styio-platform-mirror", "--all-containers", "--tail=200", "--prefix"],
    ]
    for command in diagnostic_commands:
        run(command, check=False)


def rollout_status(namespace: str, release: str, target: str) -> None:
    try:
        run(["kubectl", "-n", namespace, "rollout", "status", target, "--timeout=180s"])
    except subprocess.CalledProcessError:
        dump_rollout_diagnostics(namespace, release)
        raise


def pid_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def stop_recorded_port_forwards(state: Path) -> None:
    for pid_file in state.glob("*.pid"):
        try:
            pid = int(pid_file.read_text(encoding="utf-8").strip())
        except ValueError:
            pid_file.unlink(missing_ok=True)
            continue
        if pid_alive(pid):
            os.kill(pid, signal.SIGTERM)
        pid_file.unlink(missing_ok=True)


def start_port_forward(state: Path, namespace: str, name: str, target: str, local_port: int, remote_port: int) -> int:
    state.mkdir(parents=True, exist_ok=True)
    log_path = state / f"{name}.log"
    log = log_path.open("a", encoding="utf-8")
    proc = subprocess.Popen(
        ["kubectl", "-n", namespace, "port-forward", target, f"{local_port}:{remote_port}"],
        cwd=ROOT,
        text=True,
        stdout=log,
        stderr=log,
        start_new_session=True,
    )
    time.sleep(2)
    if proc.poll() is not None:
        log.close()
        raise RuntimeError(f"port-forward {name} exited early; see {log_path}")
    (state / f"{name}.pid").write_text(f"{proc.pid}\n", encoding="utf-8")
    (state / f"{name}.port").write_text(f"{local_port}\n", encoding="utf-8")
    log.close()
    return local_port


def wait_health(port: int) -> dict[str, object]:
    deadline = time.time() + 120
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            request = Request(
                f"http://127.0.0.1:{port}/api/styio-platform/v1/health",
                headers=IDENTITY_HEADER,
                method="GET",
            )
            with urlopen(request, timeout=5) as response:
                return json.loads(response.read().decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            last_error = exc
            time.sleep(2)
    raise RuntimeError(f"primary health check timed out: {last_error}")


def up(args: argparse.Namespace) -> int:
    require_tools(args.image_builder)
    state = state_dir(args.cluster, args.namespace, args.release)
    state.mkdir(parents=True, exist_ok=True)

    if args.build_image:
        image_archive = build_oci_image(args.image_builder, args.image)
    else:
        image_archive = None

    ensure_cluster(args.cluster)
    if image_archive is not None:
        load_image(args.cluster, image_archive)

    postgres_password = secrets.token_urlsafe(24)
    helm_install(args, postgres_password)
    wait_rollouts(args.namespace, args.release)

    endpoints: dict[str, str] = {}
    if args.port_forward:
        stop_recorded_port_forwards(state)
        primary_port = reserve_port(args.primary_port)
        mirror_port = reserve_port(args.mirror_port)
        start_port_forward(
            state,
            args.namespace,
            "primary",
            f"svc/{args.release}-styio-platform-primary",
            primary_port,
            8787,
        )
        start_port_forward(
            state,
            args.namespace,
            "mirror",
            f"svc/{args.release}-styio-platform-mirror",
            mirror_port,
            8080,
        )
        health = wait_health(primary_port)
        endpoints = {
            "primary": f"http://127.0.0.1:{primary_port}/api/styio-platform/v1",
            "mirror_registry": f"http://127.0.0.1:{mirror_port}",
        }
        write_yaml_mapping(endpoint_state_path(state), endpoints)
        workgroup_result = None
        if args.auto_register_workgroup:
            workgroup_result = self_register_workgroup(args, endpoints)
            write_yaml_mapping(
                workgroup_state_path(state),
                {
                    "workgroup_id": args.workgroup_id,
                    "cluster_id": args.workgroup_cluster_id,
                    "status": "registered",
                    "primary": endpoints["primary"],
                },
            )
        print(json.dumps({"status": "ready", "health": health, "endpoints": endpoints, "workgroup": workgroup_result}, indent=2))
    else:
        print(json.dumps({"status": "ready", "port_forward": "disabled"}, indent=2))

    print()
    print("Next commands:")
    print(f"  ./scripts/styio-platform dev status --cluster {args.cluster} --namespace {args.namespace} --release {args.release}")
    print(f"  ./scripts/styio-platform dev shell --namespace {args.namespace} --release {args.release}")
    print(f"  ./scripts/styio-platform dev logs --namespace {args.namespace} --release {args.release}")
    print(f"  ./scripts/styio-platform dev workgroup-status --cluster {args.cluster} --namespace {args.namespace} --release {args.release}")
    print(f"  ./scripts/styio-platform dev down --cluster {args.cluster} --namespace {args.namespace} --release {args.release}")
    return 0


def status(args: argparse.Namespace) -> int:
    state = state_dir(args.cluster, args.namespace, args.release)
    endpoints = load_recorded_endpoints(state)
    if endpoints:
        print(writeable_yaml_preview(endpoints).rstrip())
    else:
        print("no recorded local endpoints")
    if endpoints.get("primary"):
        try:
            workgroup = list_workgroup(endpoints["primary"], args.workgroup_id)
            print(json.dumps({"workgroup": workgroup["payload"]}, indent=2))
        except RuntimeError as error:
            print(f"workgroup status unavailable: {error}")
    run(["kubectl", "-n", args.namespace, "get", "pods,svc,pvc"], check=False)
    for pid_file in sorted(state.glob("*.pid")):
        pid = int(pid_file.read_text(encoding="utf-8").strip())
        print(f"{pid_file.stem} port-forward pid={pid} alive={pid_alive(pid)}")
    return 0


def peer_cluster_payload(peer_url: str, peer_self: dict[str, object], args: argparse.Namespace) -> dict[str, object]:
    payload = peer_self.get("payload", {})
    if not isinstance(payload, dict):
        payload = {}
    node_id = str(payload.get("node_id") or "peer")
    region = str(payload.get("region") or args.region)
    roles = payload.get("roles")
    if not isinstance(roles, list):
        roles = ["control-plane"]
    return {
        "cluster_id": node_id.replace("_", "-").lower(),
        "region": region,
        "node_id": node_id,
        "control_plane_endpoint": normalize_api_url(peer_url),
        "roles": [str(role) for role in roles],
        "trust_domain": args.workgroup_trust_domain,
        "registration_token": args.workgroup_registration_token,
        "labels": {
            "source": "dev-env-peer",
            "joined_by": args.workgroup_cluster_id,
        },
    }


def join_workgroup(args: argparse.Namespace) -> int:
    state = state_dir(args.cluster, args.namespace, args.release)
    endpoints = load_recorded_endpoints(state)
    local_primary = args.primary_url or endpoints.get("primary", "")
    if not local_primary:
        raise SystemExit("local primary endpoint is not recorded; run dev up with port forwarding or pass --primary-url")

    joined: list[dict[str, object]] = []
    self_payload = workgroup_cluster_payload(args, endpoints, primary_url=local_primary)
    joined.append({"target": "local", "response": register_cluster(local_primary, args.workgroup_id, self_payload)})

    for peer in args.peer or []:
        peer_url = normalize_api_url(peer)
        peer_self = request_json(peer_url, "/nodes/self")
        peer_payload = peer_cluster_payload(peer_url, peer_self, args)
        joined.append({"target": "local", "peer": peer_url, "response": register_cluster(local_primary, args.workgroup_id, peer_payload)})
        joined.append({"target": "peer", "peer": peer_url, "response": register_cluster(peer_url, args.workgroup_id, self_payload)})

    workgroup = list_workgroup(local_primary, args.workgroup_id)
    write_yaml_mapping(
        workgroup_state_path(state),
        {
            "workgroup_id": args.workgroup_id,
            "cluster_id": args.workgroup_cluster_id,
            "primary": local_primary,
            "joined_peers": len(args.peer or []),
            "registered_clusters": len(workgroup.get("payload", {}).get("clusters", [])),
        },
    )
    print(json.dumps({"joined": joined, "workgroup": workgroup.get("payload", {})}, indent=2))
    return 0


def workgroup_status(args: argparse.Namespace) -> int:
    state = state_dir(args.cluster, args.namespace, args.release)
    endpoints = load_recorded_endpoints(state)
    primary = args.primary_url or endpoints.get("primary", "")
    if not primary:
        raise SystemExit("local primary endpoint is not recorded; run dev up with port forwarding or pass --primary-url")
    print(json.dumps(list_workgroup(primary, args.workgroup_id), indent=2))
    return 0


def logs(args: argparse.Namespace) -> int:
    component = args.component
    if component == "primary":
        target = f"deploy/{args.release}-styio-platform-primary"
    elif component == "worker":
        target = f"deploy/{args.release}-styio-platform-worker"
    elif component == "mirror":
        target = f"deploy/{args.release}-styio-platform-mirror"
    elif component == "postgres":
        target = f"statefulset/{args.release}-styio-platform-postgres"
    else:
        raise AssertionError(component)
    cmd = ["kubectl", "-n", args.namespace, "logs", target, "--tail", str(args.tail)]
    if args.follow:
        cmd.append("-f")
    return run(cmd, check=False).returncode


def shell(args: argparse.Namespace) -> int:
    target = f"deploy/{args.release}-styio-platform-worker" if args.component == "worker" else f"deploy/{args.release}-styio-platform-primary"
    return run(["kubectl", "-n", args.namespace, "exec", "-it", target, "--", args.shell], check=False).returncode


def down(args: argparse.Namespace) -> int:
    state = state_dir(args.cluster, args.namespace, args.release)
    stop_recorded_port_forwards(state)
    run(["helm", "-n", args.namespace, "uninstall", args.release], check=False)
    run(["kubectl", "delete", "namespace", args.namespace], check=False)
    if args.delete_cluster:
        run_kind(["kind", "delete", "cluster", "--name", args.cluster], check=False)
    print("styio-platform dev environment stopped")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Manage a one-command styio-platform development environment.")
    sub = parser.add_subparsers(dest="command", required=True)

    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--config", default=str(DEFAULT_CONFIG.relative_to(ROOT)), help="YAML tool config path.")
    common.add_argument("--cluster")
    common.add_argument("--namespace")
    common.add_argument("--release")
    common.add_argument("--region")
    common.add_argument("--primary-url", help="Recorded or explicit platform primary API base URL.")
    common.add_argument("--workgroup-id")
    common.add_argument("--workgroup-cluster-id")
    common.add_argument("--workgroup-trust-domain")
    common.add_argument("--workgroup-registration-policy")
    common.add_argument("--workgroup-registration-tenant")
    common.add_argument("--workgroup-registration-token")

    up_parser = sub.add_parser("up", parents=[common], help="Build and start the full local platform dev environment.")
    up_parser.add_argument("--image")
    up_parser.add_argument("--image-builder", choices=["podman", "buildah"])
    up_parser.add_argument("--build-image", dest="build_image", action=argparse.BooleanOptionalAction, default=None)
    up_parser.add_argument("--skip-build", dest="build_image", action="store_false", default=None, help="Alias for --no-build-image.")
    up_parser.add_argument("--port-forward", dest="port_forward", action=argparse.BooleanOptionalAction, default=None)
    up_parser.add_argument("--auto-register-workgroup", action=argparse.BooleanOptionalAction, default=None)
    up_parser.add_argument("--primary-port", type=int)
    up_parser.add_argument("--mirror-port", type=int)
    up_parser.set_defaults(func=up)

    status_parser = sub.add_parser("status", parents=[common], help="Show pods, services, PVCs, and local endpoints.")
    status_parser.set_defaults(func=status)

    join_parser = sub.add_parser("join-workgroup", parents=[common], help="Register this dev environment and peer environments into one workgroup.")
    join_parser.add_argument("--peer", action="append", help="Peer primary API base URL to join bidirectionally.")
    join_parser.set_defaults(func=join_workgroup)

    workgroup_status_parser = sub.add_parser("workgroup-status", parents=[common], help="Show registered clusters in the local workgroup.")
    workgroup_status_parser.set_defaults(func=workgroup_status)

    logs_parser = sub.add_parser("logs", parents=[common], help="Show platform component logs.")
    logs_parser.add_argument("--component", choices=["primary", "worker", "mirror", "postgres"], default="primary")
    logs_parser.add_argument("--tail", type=int, default=120)
    logs_parser.add_argument("-f", "--follow", action="store_true")
    logs_parser.set_defaults(func=logs)

    shell_parser = sub.add_parser("shell", parents=[common], help="Open a shell inside a dev environment pod.")
    shell_parser.add_argument("--component", choices=["primary", "worker"], default="worker")
    shell_parser.add_argument("--shell", default="/bin/sh")
    shell_parser.set_defaults(func=shell)

    down_parser = sub.add_parser("down", parents=[common], help="Stop the dev environment.")
    down_parser.add_argument("--delete-cluster", action=argparse.BooleanOptionalAction, default=None)
    down_parser.set_defaults(func=down)

    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        apply_tool_config(args)
    except YamlConfigError as exc:
        parser.error(str(exc))
    try:
        return int(args.func(args))
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
