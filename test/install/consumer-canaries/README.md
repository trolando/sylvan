# Installed consumer canaries

These are small out-of-tree consumers of the installed Sylvan package. They
exercise advanced integration seams used by Storm, mCRL2, and LTSmin without
copying those clients or presenting the canaries as ordinary API examples.

Each executable accepts a worker count. CTest runs the relevant canaries with
one and two Lace workers, and each canary forces garbage collection at a
consumer-sensitive boundary.
