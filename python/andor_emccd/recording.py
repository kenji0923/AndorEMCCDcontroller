"""Read raw frames saved by the GUI's Stream recording (BigTIFF).

Requires the optional ``tifffile`` package: pip install 'andor-emccd[recording]'.
Iterates page by page, so the whole recording need not fit in memory.
"""

import json


def iter_recorded_frames(path):
    """Yield (numpy image, metadata dict) from a completed recording.

    metadata['frame'] contains seq, timestamp, dtype, ROI, exposure, etc.
    metadata['recording'] contains sampling and loss counters at selection.
    The first page also contains metadata['metadata'] (initial settings).
    Pages may have different shapes or pixel types after camera changes.
    """
    import tifffile

    with tifffile.TiffFile(path) as recording:
        for page in recording.pages:
            yield page.asarray(), json.loads(page.description)
