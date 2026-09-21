# Qidi Admin Studio

This directory contains Qidi-specific integration code for the AGPL-3.0
Qidi Admin Studio fork of OrcaSlicer. The native Orca prepare and preview
workflows remain intact; the integration adds a secure hand-off to the
user-owned Qidi Admin Raspberry gateway after slicing.

## Product boundary

- Orca owns model preparation, project files, slicing and G-code preview.
- Qidi Admin owns printer control, live camera, material inventory, command
  history and the Raspberry gateway.
- The slicer never stores a Raspberry API key in a `.3mf` project or generated
  G-code. Credentials belong to the local secure application configuration.

## Windows releases without local C++ compilation

The repository workflow `.github/workflows/qidi-admin-studio-windows.yml`
builds the x64 package in GitHub Actions. Its dependency and compiler caches
are restored by Orca's existing reusable build workflow, so a developer or
user downloads the resulting artifact instead of compiling locally.

Before enabling the workflow in a GitHub fork:

1. Create a GitHub repository from this source and make it public to meet the
   AGPL-3.0 distribution obligations.
2. Push the `main` branch.
3. Open **Actions → Qidi Admin Studio — Windows → Run workflow**.
4. Run once with `build_deps_only=true` to populate the dependency cache.
5. Run it again normally and download the Windows artifact from the completed
   job.

The first CI dependency build is intentionally slow. It runs on the hosted
runner, not on the user's PC. Later builds restore the cache and only compile
the changed native files where possible.

## Integration contract

The future native hand-off must use the existing authenticated Raspberry
endpoint rather than direct public Moonraker access:

```text
POST /api/v1/printer/preflight?filename=<generated-gcode>
POST /api/v1/files/upload
POST /api/v1/printer/start
```

The actual upload route and response shape must be confirmed against the
deployed server before wiring the GUI action. A failed preflight must present
the server's material/camera/maintenance reason and must not start a print.
