"""Check migration metadata and optionally all pinned Git source objects."""

import argparse
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path, PurePosixPath

BASELINES = {
    "legacy": "e54047b51519fa449dfe4a338af8d126bed8fac4",
    "forge": "ca5219e7cf6929749b699d8d82da33cdc4eb463e",
}
ASSET_STATES = {"not_migrated", "in_progress", "migrated", "blocked", "deferred"}
VERIFY_STATES = {"not_run", "failed", "passed"}
BATCH_STATES = {"pending", "in_progress", "done", "blocked", "deferred"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def git(repo, *args):
    result = subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True,
        check=False,
    )
    if result.returncode:
        raise ValueError(f"Git check failed in {repo}: {' '.join(args)}")
    return result.stdout


def index(rows, label):
    require(isinstance(rows, list), f"{label} must be a list")
    result = {}
    for row in rows:
        ident = row["id"]
        require(isinstance(ident, str) and ident, f"Invalid {label} ID")
        require(ident not in result, f"Duplicate {label} ID: {ident}")
        result[ident] = row
    return result


def local_file(root, path):
    require(isinstance(path, str) and path, "Empty target/evidence path")
    rel = PurePosixPath(path)
    require(
        not rel.is_absolute() and ".." not in rel.parts and "\\" not in path,
        f"Unsafe path: {path}",
    )
    file = root / path
    require(file.resolve().is_relative_to(root.resolve()), f"Escaping path: {path}")
    require(file.is_file(), f"Missing target/evidence file: {path}")
    return file


def source_tree(repo, commit):
    entries = {}
    for raw in git(repo, "ls-tree", "-rz", "--full-tree", commit).split(b"\0"):
        if raw:
            meta, path = raw.split(b"\t", 1)
            entries[path.decode()] = tuple(meta.decode().split())
    return entries


def validate(data, root, sources=False):
    require(data["schema_version"] == 1, "Unsupported schema version")
    require(
        data["baseline_tag"] == "legacy-baseline-2026-11-08", "Baseline tag changed"
    )
    repos = index(data["repositories"], "repository")
    require(set(repos) == set(BASELINES), "Unexpected source repositories")
    for ident, commit in BASELINES.items():
        require(repos[ident]["commit"] == commit, f"Pinned commit changed: {ident}")
        require(
            re.fullmatch(r"[0-9a-f]{40}", repos[ident]["tree"]) is not None,
            f"Invalid pinned tree: {ident}",
        )
    modules = index(data["modules"], "module")
    assets = index(data["assets"], "asset")
    batches = index(data["batches"], "batch")
    decisions = index(data["design_decisions"], "decision")
    require(
        set(decisions) == {f"D{i:02}" for i in range(1, 13)}, "Missing design decisions"
    )
    for decision in decisions.values():
        require(
            decision["status"]
            in {"needs_owner_review", "direction_approved", "approved", "rejected"},
            f"Invalid decision state: {decision['id']}",
        )
        if decision["status"] != "needs_owner_review":
            local_file(root, decision["record_path"])
        if decision["status"] == "direction_approved":
            pending = decision["pending_details"]
            require(
                isinstance(pending, list)
                and pending
                and all(isinstance(item, str) and item.strip() for item in pending),
                f"Missing pending decision details: {decision['id']}",
            )

    visiting, visited = set(), set()

    def visit(ident):
        require(ident in batches, f"Missing batch dependency: {ident}")
        require(ident not in visiting, f"Dependency cycle at {ident}")
        if ident in visited:
            return
        batch = batches[ident]
        require(batch["status"] in BATCH_STATES, f"Invalid batch state: {ident}")
        require(
            len(set(batch["depends_on"])) == len(batch["depends_on"]),
            f"Duplicate batch dependency: {ident}",
        )
        visiting.add(ident)
        for prerequisite in batch["depends_on"]:
            visit(prerequisite)
            if batch["status"] in {"in_progress", "done"}:
                require(
                    batches[prerequisite]["status"] == "done",
                    f"Unfinished prerequisite for {ident}: {prerequisite}",
                )
        visiting.remove(ident)
        visited.add(ident)
        if batch["status"] == "done":
            # E-1（D4 授权 P3 自主整改）：done 证据机器化——不再接受
            # 仅改状态字段的"口头完成"。evidence_paths 必须非空、实际
            # 存在且落在 docs/**（证据面）；implementation_commit 必填
            # 且为严格 commit 对象（cat-file -t——annotated tag 的
            # ^{commit} 剥壳不能冒充）且是 HEAD 祖先（review P2-2/P2-3/
            # P3-8：任意 legacy 历史对象同库不可通过）。
            require(
                isinstance(batch.get("evidence_paths"), list)
                and batch["evidence_paths"],
                f"done batch missing evidence paths: {ident}",
            )
            for path in batch["evidence_paths"]:
                local_file(root, path)
                require(
                    path.startswith("docs/"),
                    f"done evidence outside docs/: {ident}: {path}",
                )
            commit = batch.get("implementation_commit")
            require(
                isinstance(commit, str)
                and re.fullmatch(r"[0-9a-f]{7,40}", commit) is not None,
                f"done batch missing/malformed implementation commit: {ident}",
            )
            require(
                subprocess.run(
                    ["git", "-C", str(root), "cat-file", "-t", commit],
                    capture_output=True,
                    check=False,
                ).stdout.strip()
                == b"commit",
                f"implementation commit is not a commit object: {ident}: {commit}",
            )
            require(
                subprocess.run(
                    ["git", "-C", str(root), "merge-base", "--is-ancestor",
                     commit, "HEAD"],
                    capture_output=True,
                    check=False,
                ).returncode
                == 0,
                f"implementation commit not an ancestor of HEAD: {ident}: {commit}",
            )

    for ident in batches:
        visit(ident)
    for module in modules.values():
        require(
            module["first_batch"] in batches, f"Missing module batch: {module['id']}"
        )
        require(
            module["status"] in {"planned", "in_progress", "implemented", "deferred"},
            f"Invalid module status: {module['id']}",
        )
        rel = PurePosixPath(module["planned_root"])
        require(
            not rel.is_absolute() and ".." not in rel.parts and str(rel) != ".",
            f"Invalid module root: {module['id']}",
        )

    files = index(data["source_files"], "source file")
    seen = set()
    counts = dict.fromkeys(assets, 0)
    for file in files.values():
        repo, path = file["repository"], file["path"]
        require(repo in repos, f"Unknown source repository: {repo}")
        require((repo, path) not in seen, f"Duplicate source path: {repo}:{path}")
        seen.add((repo, path))
        expected_id = "S-" + repo + "-" + hashlib.sha256(path.encode()).hexdigest()[:20]
        require(file["id"] == expected_id, f"Source ID changed: {repo}:{path}")
        require(file["asset_id"] in assets, f"Missing asset for {repo}:{path}")
        asset = assets[file["asset_id"]]
        require(
            asset["source_repository"] == repo, f"Asset repository mismatch: {path}"
        )
        selector = asset["source_selector"]
        require(
            selector["kind"] in {"exact", "prefix"}, f"Invalid selector: {asset['id']}"
        )
        require(
            path == selector["path"]
            if selector["kind"] == "exact"
            else path.startswith(selector["path"]),
            f"Source selector mismatch: {path}",
        )
        counts[file["asset_id"]] += 1
        require(file["git_type"] in {"blob", "commit"}, f"Invalid Git type: {path}")
        require(
            file["mode"] in {"100644", "100755", "120000", "160000"},
            f"Invalid mode: {path}",
        )
        require(
            (file["git_type"] == "commit") == (file["mode"] == "160000"),
            f"Git type/mode mismatch: {path}",
        )
        require(
            re.fullmatch(r"[0-9a-f]{40}", file["git_object"]) is not None,
            f"Invalid object ID: {path}",
        )
        require(
            file["sha256"] is None
            if file["git_type"] == "commit"
            else isinstance(file["sha256"], str)
            and re.fullmatch(r"[0-9a-f]{64}", file["sha256"]) is not None,
            f"Invalid SHA256: {path}",
        )
        require(
            file["disposition"]
            in {"pending_review", "migrate", "reference", "defer", "discard"},
            f"Invalid disposition: {path}",
        )
        if file["disposition"] == "migrate":
            require(file["target_paths"], f"Missing migration target: {path}")
            commit = file["migration_commit"]
            require(
                isinstance(commit, str)
                and re.fullmatch(r"[0-9a-f]{40}", commit) is not None,
                f"Missing migration commit: {path}",
            )
            require(
                git(root, "cat-file", "-t", commit).strip() == b"commit",
                f"Migration object is not a commit: {path}",
            )
            for target in file["target_paths"]:
                local_file(root, target)
                require(
                    source_tree(root, commit).get(target) is not None,
                    f"Target absent from migration commit: {target}",
                )
        else:
            require(
                not file["target_paths"] and file["migration_commit"] is None,
                f"Non-migrated source has migration data: {path}",
            )

    for ident, asset in assets.items():
        require(asset["first_batch"] in batches, f"Missing asset batch: {ident}")
        require(asset["status"] in ASSET_STATES, f"Invalid asset status: {ident}")
        require(asset["constraints"].strip(), f"Missing asset constraints: {ident}")
        require(
            asset["source_file_count"] == counts[ident],
            f"Asset source count mismatch: {ident}",
        )
        require(
            asset["target_modules"] and set(asset["target_modules"]) <= set(modules),
            f"Unknown/empty target modules: {ident}",
        )
        verification = asset["verification"]
        require(
            verification["state"] in VERIFY_STATES,
            f"Invalid verification state: {ident}",
        )
        for path in verification["evidence_paths"]:
            local_file(root, path)
        if verification["state"] == "passed":
            require(verification["evidence_paths"], f"Missing passed evidence: {ident}")
            require(
                asset["status"] == "migrated",
                f"Unmigrated asset marked passed: {ident}",
            )
        if asset["source_repository"] is None:
            require(
                asset["migration_action"] == "new"
                and asset["source_selector"] is None
                and counts[ident] == 0,
                f"New asset has old source: {ident}",
            )
        else:
            require(
                asset["source_repository"] in repos and counts[ident] > 0,
                f"Source asset has no known source: {ident}",
            )
        if asset["status"] == "migrated":
            selected = [f for f in files.values() if f["asset_id"] == ident]
            require(
                all(f["disposition"] != "pending_review" for f in selected),
                f"Unreviewed source in migrated asset: {ident}",
            )
            require(
                any(f["disposition"] == "migrate" for f in selected)
                or asset["target_paths"],
                f"Migrated asset has no output: {ident}",
            )
            for target in asset["target_paths"]:
                local_file(root, target)
            if asset["source_repository"] is None:
                commit = asset.get("implementation_commit")
                require(
                    isinstance(commit, str)
                    and re.fullmatch(r"[0-9a-f]{40}", commit) is not None,
                    f"Missing new implementation commit: {ident}",
                )
                require(
                    git(root, "cat-file", "-t", commit).strip() == b"commit",
                    f"New implementation object is not a commit: {ident}",
                )
                tree = source_tree(root, commit)
                for target in asset["target_paths"]:
                    require(
                        target in tree,
                        f"New target absent from implementation commit: {target}",
                    )

    gitlink = next(
        (
            f
            for f in files.values()
            if f["repository"] == "legacy" and f["path"] == "agent"
        ),
        None,
    )
    require(
        gitlink is not None
        and gitlink["git_type"] == "commit"
        and gitlink["git_object"] == BASELINES["forge"],
        "Forge gitlink mismatch",
    )
    if sources:
        for ident, repo in repos.items():
            source = (root / repo["local_path"]).resolve()
            tree_id = (
                git(source, "rev-parse", repo["commit"] + "^{tree}").decode().strip()
            )
            require(tree_id == repo["tree"], f"Pinned tree mismatch: {ident}")
            tree = source_tree(source, repo["commit"])
            selected = [f for f in files.values() if f["repository"] == ident]
            require(
                set(tree) == {f["path"] for f in selected},
                f"Source coverage mismatch: {ident}",
            )
            for file in selected:
                require(
                    tree[file["path"]]
                    == (file["mode"], file["git_type"], file["git_object"]),
                    f"Source identity mismatch: {ident}:{file['path']}",
                )
                if file["git_type"] == "blob":
                    digest = hashlib.sha256(
                        git(source, "cat-file", "blob", file["git_object"])
                    ).hexdigest()
                    require(
                        digest == file["sha256"],
                        f"Source digest mismatch: {ident}:{file['path']}",
                    )
        source = (root / repos["legacy"]["local_path"]).resolve()
        require(
            git(source, "rev-parse", data["baseline_tag"] + "^{commit}")
            .decode()
            .strip()
            == BASELINES["legacy"],
            "Source baseline tag mismatch",
        )
    return len(files), len(assets)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument(
        "--sources",
        action="store_true",
        help="Verify complete pinned source Git trees and blob digests",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    path = args.manifest or root / "docs/migration-assets.json"
    try:
        files, assets = validate(json.loads(path.read_text()), root, args.sources)
    except (OSError, ValueError, KeyError, TypeError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    suffix = "including pinned source objects" if args.sources else "metadata only"
    print(f"PASS: {files} source entries, {assets} assets; {suffix}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
