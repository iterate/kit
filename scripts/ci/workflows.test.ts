// What the workflows must keep doing, read from their YAML. Moved from iterate's
// scripts/ci/depot-workflows.test.ts at iterate/iterate@a5a07e8, with Kit.
import { readFileSync } from "node:fs";
import { matchesGlob, resolve } from "node:path";
import { expect, test } from "vitest";
import { parse as parseYaml } from "yaml";

// The legs run the firmware's own CMake, third-party components and scripts; none of them may hold
// a token that can create a release (scripts/firmware-release.ts).
test("Kit Firmware publishes from one job that runs no repository code", () => {
  const workflow = loadWorkflow("kit-firmware.yml");
  const writers = Object.entries(workflow.jobs).filter(
    ([, job]: [string, any]) => job.permissions?.contents === "write",
  );
  const publish = workflow.jobs["publish-firmware"];
  const checkouts = Object.values(workflow.jobs).flatMap((job: any) =>
    (job.steps || []).filter((step: any) => step.uses?.startsWith("actions/checkout")),
  );

  expect(workflow.permissions).toEqual({ contents: "read" });
  expect(writers.map(([jobId]) => jobId)).toEqual(["publish-firmware"]);
  expect(publish.steps.filter((step: any) => step.uses?.startsWith("actions/checkout"))).toEqual(
    [],
  );
  expect(checkouts.length).toBeGreaterThan(0);
  for (const checkout of checkouts) expect(checkout.with?.["persist-credentials"]).toBe(false);
  // each board has its own newest release; none is the repository's Latest
  expect(publish.steps.map((step: any) => step.run || "").join("\n")).toContain("--latest=false");
  expect(workflow.on.push.paths).toEqual(workflow.on.pull_request.paths);
  // the schedule is the bounded recovery for a failed publish
  expect(workflow.on.schedule).toEqual([{ cron: expect.any(String) }]);
});

// A leg that installs ESP-IDF itself makes a GitHub clone and a PyPI install, any of whose
// downloads can fail a board with no firmware change. So Depot Cache holds it, keyed by its pin
// (scripts/ci/esp-idf.sh), and a main leg saves it, as installed, for a pin that has none yet.
test("Kit Firmware legs take ESP-IDF from Depot Cache, keyed by its pin", () => {
  const workflow = loadWorkflow("kit-firmware.yml");
  const leg = workflow.jobs["build-firmware"];
  const steps: any[] = leg.steps;
  const index = (name: string) => steps.findIndex((step) => step.name === name);
  const paths = "/home/runner/esp-idf\n/home/runner/.espressif\n";

  expect(leg["runs-on"]).toBe("depot-ubuntu-24.04-4");
  // the pin's hash and python3's version (scripts/ci/esp-idf.test.ts)
  expect(steps[index("ESP-IDF's key")]).toMatchObject({
    id: "esp-idf-key",
    run: 'scripts/ci/esp-idf.sh key >>"$GITHUB_OUTPUT"',
  });
  expect(steps[index("Restore ESP-IDF")]).toMatchObject({
    id: "esp-idf",
    uses: "actions/cache/restore@v4",
    with: { path: paths, key: "${{ steps.esp-idf-key.outputs.key }}" },
  });
  // an older pin's ESP-IDF is of no use to this one
  expect(steps[index("Restore ESP-IDF")].with["restore-keys"]).toBeUndefined();
  expect(steps[index("ESP-IDF")]).toMatchObject({
    id: "ensure",
    run: "scripts/ci/esp-idf.sh ensure",
  });
  expect(steps[index("Save ESP-IDF")]).toMatchObject({
    if: "${{ github.ref == 'refs/heads/main' && steps.ensure.outcome == 'success' && steps.esp-idf.outputs.cache-hit != 'true' }}",
    uses: "actions/cache/save@v4",
    with: { path: paths },
  });
  expect(index("ESP-IDF's key")).toBe(index("Restore ESP-IDF") - 1);
  expect(index("Restore ESP-IDF")).toBeLessThan(index("ESP-IDF"));
  expect(index("Save ESP-IDF")).toBe(index("ESP-IDF") + 1);
  expect(index("Save ESP-IDF")).toBeLessThan(index("Build"));
  expect(
    steps.map((step) => step.run || "").filter((run) => /git clone|install\.sh/.test(run)),
  ).toEqual([]);
  expect(workflow.on.pull_request.paths).toEqual(
    expect.arrayContaining(["scripts/ci/esp-idf.sh", "scripts/ci/toolchain.sh"]),
  );
});

// README.md "Firmware releases": the Worker streams firmware from GitHub releases
test("Deploy ships only the installer, preview first; firmware ships as GitHub releases", () => {
  const workflow = loadWorkflow("deploy.yml");
  const paths: string[] = workflow.on.push.paths;
  const runs = Object.values(workflow.jobs).flatMap((job: any) =>
    job.steps.map((step: any) => step.run || ""),
  );

  expect(runs.filter((run) => /esp-idf|export\.sh|firmware:/i.test(run))).toEqual([]);
  expect(triggers(paths, "firmware/targets/havpe/CMakeLists.txt")).toBe(false);
  expect(triggers(paths, "src/firmware/catalog.ts")).toBe(true);
  expect(triggers(paths, "pnpm-lock.yaml")).toBe(true);
  expect(workflow.jobs.prd.needs).toBe("preview");
  expect(workflow.concurrency).toEqual({ group: "deploy-main", "cancel-in-progress": false });
});

function loadWorkflow(file: string): any {
  return parseYaml(
    readFileSync(resolve(import.meta.dirname, "../../.depot/workflows", file), "utf8"),
  );
}

/** Whether a push of `file` runs a workflow with these `paths`: the last pattern that matches wins. */
function triggers(paths: string[], file: string) {
  let included = false;
  for (const pattern of paths) {
    const negated = pattern.startsWith("!");
    if (matchesGlob(file, negated ? pattern.slice(1) : pattern)) included = !negated;
  }
  return included;
}
