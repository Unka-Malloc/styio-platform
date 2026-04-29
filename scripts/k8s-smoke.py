#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import secrets
import shutil
import socket
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path
from urllib.request import Request, urlopen

from styio_yaml import YamlConfigError, load_config_section


ROOT = Path(__file__).resolve().parents[1]
CHART = ROOT / "deploy/helm/styio-platform"
DEFAULT_CONFIG = ROOT / "config/styio-platform-tools.yaml"
K8S_SMOKE_DEFAULTS: dict[str, object] = {
    "cluster": "styio-platform-smoke",
    "namespace": "styio-platform-smoke",
    "release": "styio",
    "image": "styio-platform:smoke",
    "image_builder": "podman",
    "keep_cluster": False,
    "region": "local-dev",
    "workgroup_id": "styio-local-workgroup",
    "workgroup_cluster_id": "styio-platform-smoke",
    "workgroup_trust_domain": "styio-platform-local",
    "workgroup_registration_policy": "local-dev-default",
    "workgroup_registration_tenant": "platform",
    "workgroup_registration_token": "styio-local-dev-token",
}
IDENTITY_HEADER = {
    "X-Styio-Mtls-Uri-San": "spiffe://styio-platform/tenant/platform/role/operator/node/k8s-smoke",
}


def run(
    cmd: list[str],
    *,
    input_text: str | None = None,
    check: bool = True,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(
        cmd,
        cwd=ROOT,
        text=True,
        input=input_text,
        check=check,
        capture_output=capture,
    )


def require_tools(builder: str) -> None:
    required = ["kind", "kubectl", "helm", "podman"]
    if builder == "buildah":
        required.append("buildah")
    missing = [tool for tool in required if shutil.which(tool) is None]
    if missing:
        raise SystemExit(f"missing required open k8s smoke tools: {', '.join(missing)}")


def kind_env() -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("KIND_EXPERIMENTAL_PROVIDER", "podman")
    return env


def run_kind(cmd: list[str], *, check: bool = True, capture: bool = False) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(
        cmd,
        cwd=ROOT,
        text=True,
        check=check,
        capture_output=capture,
        env=kind_env(),
    )


def resolve_config_path(raw_path: str | None) -> Path:
    path = Path(raw_path) if raw_path else DEFAULT_CONFIG
    if not path.is_absolute():
        path = ROOT / path
    return path


def typed_config_value(path: Path, key: str, value: object, fallback: object) -> object:
    if isinstance(fallback, bool):
        if not isinstance(value, bool):
            raise YamlConfigError(f"{path}: k8s_smoke.{key} must be a boolean")
        return value
    if isinstance(fallback, str):
        if not isinstance(value, str):
            raise YamlConfigError(f"{path}: k8s_smoke.{key} must be a string")
        return value
    return value


def apply_tool_config(args: argparse.Namespace) -> None:
    config_path = resolve_config_path(args.config)
    required = config_path != DEFAULT_CONFIG
    config = load_config_section(config_path, "k8s_smoke", required=required)
    for key, fallback in K8S_SMOKE_DEFAULTS.items():
        cli_value = getattr(args, key, None)
        if cli_value is not None:
            continue
        config_value = config.get(key, fallback)
        setattr(args, key, typed_config_value(config_path, key, config_value, fallback))
    if args.image_builder not in {"podman", "buildah"}:
        raise YamlConfigError(f"{config_path}: k8s_smoke.image_builder must be podman or buildah")


def build_oci_image(builder: str, image: str) -> Path:
    archive = Path(tempfile.mkdtemp(prefix="styio-platform-image-")) / "image.tar"
    if builder == "podman":
        run(["podman", "build", "-f", "Containerfile", "-t", image, "."])
        run(["podman", "save", "-o", str(archive), image])
        return archive
    if builder == "buildah":
        run(["buildah", "bud", "-f", "Containerfile", "-t", image, "."])
        run(["buildah", "push", image, f"docker-archive:{archive}:{image}"])
        return archive
    raise AssertionError(f"unsupported image builder: {builder}")


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def wait_http_json(url: str, *, method: str = "GET", body: dict[str, object] | None = None, timeout: int = 120) -> dict[str, object]:
    deadline = time.time() + timeout
    data = None if body is None else json.dumps(body).encode("utf-8")
    headers = {"Content-Type": "application/json", **IDENTITY_HEADER}
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            request = Request(url, data=data, headers=headers, method=method)
            with urlopen(request, timeout=5) as response:
                return json.loads(response.read().decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            last_error = exc
            time.sleep(2)
    raise AssertionError(f"timed out waiting for {url}: {last_error}")


def start_port_forward(namespace: str, target: str, remote_port: int) -> tuple[subprocess.Popen[str], int]:
    local_port = reserve_port()
    proc = subprocess.Popen(
        ["kubectl", "-n", namespace, "port-forward", target, f"{local_port}:{remote_port}"],
        cwd=ROOT,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    time.sleep(2)
    if proc.poll() is not None:
        stdout, stderr = proc.communicate()
        raise AssertionError(f"port-forward failed\n{stdout}\n{stderr}")
    return proc, local_port


def apply_fixture_git_service(namespace: str) -> None:
    manifest = """
apiVersion: v1
kind: Service
metadata:
  name: styio-platform-fixture-git
spec:
  selector:
    app: styio-platform-fixture-git
  ports:
    - name: git
      port: 9418
      targetPort: 9418
---
apiVersion: v1
kind: Pod
metadata:
  name: styio-platform-fixture-git
  labels:
    app: styio-platform-fixture-git
spec:
  restartPolicy: Always
  initContainers:
    - name: seed
      image: alpine/git:latest
      command:
        - sh
        - -c
        - |
          set -eu
          mkdir -p /repo/fixture/src
          cd /repo/fixture
          git init
          git config user.email smoke@example.invalid
          git config user.name smoke
          cat > spio.toml <<'EOF'
          [spio]
          manifest-version = 1

          [package]
          name = "demo/app"
          version = "0.1.0"
          edition = "2026"
          publish = false

          [toolchain]
          channel = "nightly"
          implicit-std = true

          [lib]
          path = "src/lib.styio"
          EOF
          printf '# smoke := 1\\n' > src/lib.styio
          git add .
          git commit -m fixture
      volumeMounts:
        - name: repo
          mountPath: /repo
  containers:
    - name: git
      image: alpine/git:latest
      command:
        - git
        - daemon
        - --reuseaddr
        - --base-path=/repo
        - --export-all
        - --verbose
        - --listen=0.0.0.0
        - --port=9418
      ports:
        - containerPort: 9418
      volumeMounts:
        - name: repo
          mountPath: /repo
  volumes:
    - name: repo
      emptyDir: {}
"""
    run(["kubectl", "-n", namespace, "apply", "-f", "-"], input_text=textwrap.dedent(manifest))
    run(["kubectl", "-n", namespace, "wait", "--for=condition=Ready", "pod/styio-platform-fixture-git", "--timeout=120s"])


def publish_registry_fixture(namespace: str, release: str) -> None:
    script = r"""
set -eu
mkdir -p /tmp/styio-platform-publish/src
cat > /tmp/styio-platform-publish/spio.toml <<'EOF'
[spio]
manifest-version = 1

[package]
name = "demo/app"
version = "0.1.0"
edition = "2026"
publish = true

[toolchain]
channel = "nightly"
implicit-std = true

[lib]
path = "src/lib.styio"
EOF
printf '# publish := 1\n' > /tmp/styio-platform-publish/src/lib.styio
curl -fsS \
  -H 'Content-Type: application/json' \
  -H 'X-Styio-Mtls-Uri-San: spiffe://styio-platform/tenant/platform/role/registry-writer/node/k8s-smoke' \
  -d '{"manifest_path":"/tmp/styio-platform-publish/spio.toml","publisher_id":"k8s-smoke"}' \
  http://127.0.0.1:8787/api/spio-registry-control/v1/publish
"""
    run([
        "kubectl",
        "-n",
        namespace,
        "exec",
        f"deploy/{release}-styio-platform-primary",
        "-c",
        "primary",
        "--",
        "sh",
        "-c",
        script,
    ])


def main() -> int:
    parser = argparse.ArgumentParser(description="Run styio-platform Helm/kind end-to-end smoke with open OCI tooling.")
    parser.add_argument("--config", default=str(DEFAULT_CONFIG.relative_to(ROOT)), help="YAML tool config path.")
    parser.add_argument("--cluster")
    parser.add_argument("--namespace")
    parser.add_argument("--release")
    parser.add_argument("--image")
    parser.add_argument("--image-builder", choices=["podman", "buildah"])
    parser.add_argument("--keep-cluster", action=argparse.BooleanOptionalAction, default=None)
    parser.add_argument("--region")
    parser.add_argument("--workgroup-id")
    parser.add_argument("--workgroup-cluster-id")
    parser.add_argument("--workgroup-trust-domain")
    parser.add_argument("--workgroup-registration-policy")
    parser.add_argument("--workgroup-registration-tenant")
    parser.add_argument("--workgroup-registration-token")
    args = parser.parse_args()
    try:
        apply_tool_config(args)
    except YamlConfigError as exc:
        parser.error(str(exc))

    require_tools(args.image_builder)
    image_repo, image_tag = args.image.rsplit(":", 1)
    postgres_password = secrets.token_urlsafe(18)

    image_archive = build_oci_image(args.image_builder, args.image)
    existing = run_kind(["kind", "get", "clusters"], check=False, capture=True)
    if args.cluster not in existing.stdout.splitlines():
        run_kind(["kind", "create", "cluster", "--name", args.cluster])
    run_kind(["kind", "load", "image-archive", str(image_archive), "--name", args.cluster])
    run(["kubectl", "create", "namespace", args.namespace], check=False)

    run([
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
    ])

    run(["kubectl", "-n", args.namespace, "rollout", "status", f"statefulset/{args.release}-styio-platform-postgres", "--timeout=180s"])
    run(["kubectl", "-n", args.namespace, "rollout", "status", f"deploy/{args.release}-styio-platform-primary", "--timeout=180s"])
    run(["kubectl", "-n", args.namespace, "rollout", "status", f"deploy/{args.release}-styio-platform-worker", "--timeout=180s"])
    run(["kubectl", "-n", args.namespace, "rollout", "status", f"deploy/{args.release}-styio-platform-mirror", "--timeout=180s"])

    apply_fixture_git_service(args.namespace)
    publish_registry_fixture(args.namespace, args.release)

    forward, port = start_port_forward(args.namespace, f"svc/{args.release}-styio-platform-primary", 8787)
    try:
        register = wait_http_json(
            f"http://127.0.0.1:{port}/api/styio-platform/v1/workgroups/{args.workgroup_id}/clusters/register",
            method="POST",
            body={
                "cluster_id": args.workgroup_cluster_id,
                "region": args.region,
                "node_id": f"{args.workgroup_cluster_id}-primary",
                "control_plane_endpoint": f"http://127.0.0.1:{port}/api/styio-platform/v1",
                "internal_control_plane_endpoint": f"http://{args.release}-styio-platform-primary.{args.namespace}.svc.cluster.local:8787/api/styio-platform/v1",
                "roles": ["control-plane", "worker", "mirror", "registry-writer"],
                "trust_domain": args.workgroup_trust_domain,
                "registration_token": args.workgroup_registration_token,
                "labels": {"source": "k8s-smoke", "namespace": args.namespace},
            },
        )
        assert register["payload"]["cluster_id"] == args.workgroup_cluster_id
        workgroup = wait_http_json(
            f"http://127.0.0.1:{port}/api/styio-platform/v1/workgroups/{args.workgroup_id}/clusters",
            timeout=10,
        )
        assert any(cluster["cluster_id"] == args.workgroup_cluster_id for cluster in workgroup["payload"]["clusters"])

        submit = wait_http_json(
            f"http://127.0.0.1:{port}/api/styio-platform/v1/jobs",
            method="POST",
            body={
                "tenant_id": "tenant-smoke",
                "workspace_id": "workspace-smoke",
                "action": "build",
                "region": "local-dev",
                "preferred_worker_pool": "linux/x86_64/build/nightly/minimal",
                "job_request": {
                    "schema_version": 1,
                    "api_path": "/api/styio-platform/v1/jobs",
                    "action": "build",
                    "manifest_path": "spio.toml",
                    "source": {"origin": "git://styio-platform-fixture-git:9418/fixture"},
                    "toolchain": {"mode": "build", "channel": "nightly", "build_mode": "minimal"},
                    "workflow": {"locked": False, "offline": False},
                    "target": {"lib": True},
                    "cloud": {},
                },
            },
        )
        job_id = submit["payload"]["job_id"]
        deadline = time.time() + 300
        status = "queued"
        while time.time() < deadline:
            job = wait_http_json(f"http://127.0.0.1:{port}/api/styio-platform/v1/jobs/{job_id}", timeout=10)
            status = job["payload"]["status"]
            if status in {"succeeded", "failed", "cancelled"}:
                if status != "succeeded":
                    raise AssertionError(f"job ended with status {status}: {json.dumps(job, indent=2)}")
                break
            time.sleep(5)
        else:
            raise AssertionError(f"timed out waiting for job completion; last status={status}")
    finally:
        forward.terminate()
        forward.wait(timeout=10)

    mirror_forward, mirror_port = start_port_forward(args.namespace, f"svc/{args.release}-styio-platform-mirror", 8080)
    try:
        deadline = time.time() + 120
        while True:
            try:
                with urlopen(f"http://127.0.0.1:{mirror_port}/config.json", timeout=5) as response:
                    assert response.status == 200
                    break
            except Exception:  # noqa: BLE001
                if time.time() > deadline:
                    raise
                time.sleep(3)
    finally:
        mirror_forward.terminate()
        mirror_forward.wait(timeout=10)

    print("styio-platform k8s smoke passed")
    if not args.keep_cluster:
        run(["kind", "delete", "cluster", "--name", args.cluster])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
