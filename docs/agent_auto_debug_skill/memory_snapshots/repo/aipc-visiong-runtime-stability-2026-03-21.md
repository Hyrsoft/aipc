# VisionG Runtime Stability Notes (Snapshot 2026-03-21)

- avoid creating/destroying scoped_interpreter for each mode switch
- keep embedded python interpreter as process-lifetime runtime
- guard UpdateCode/Deinit by state mutex
- python project process() should return ImageBuffer; do not create DisplayUDP/HTTP/RTSP inside project script
- pybind11 python-derived members may crash before interpreter init; prefer default-constructed py::object then init under GIL
- field evidence: crash moved to LoadCode -> py::exec stage after interpreter init success
