# natron-kdenlive-link
Use Natron compositions as an effect in Kdenlive, Premiere + After Effects -Dynamic Link style. An MLT filter sends frames to a local daemon, a headless Natron worker renders them, and a content-hash cache serves the results. Falls back to the original frame if Natron is slow or absent. Linux, C++ and Python, early development.
