# Python Project Semantics (Snapshot 2026-03-21)

- Python editor uses project-only flow.
- frontend uses:
  - /api/python/projects (list)
  - /api/python/projects/{name} (load/save/delete)
  - /api/python/projects/create
- deploy api accepts { project } and loads from ../python_projects.
- /api/python/templates removed.
- editor modal updated to full-height layout for desktop and mobile.
