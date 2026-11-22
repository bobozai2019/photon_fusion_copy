"""Build and publish the standalone Godot SDK."""
import sys
from build import main

if __name__ == "__main__":
    sys.argv[1:1] = ["godot", "--package"]
    main()
