"""Saves an in-game screenshot through BeamNG's MCP server: python shot.py <out.jpg> [scale]"""
import base64, sys, time
sys.path.insert(0, __file__.replace("\\", "/").rsplit("/", 1)[0])
import mcp


def shot(path, scale=0.5):
    for _ in range(6):   # a call either starts a capture or returns the last one
        res = mcp.call("screenshot_image", {"scale": scale}).get("result", {})
        for c in res.get("content", []):
            if c.get("type") == "image":
                open(path, "wb").write(base64.b64decode(c["data"]))
                return True
        time.sleep(1.5)
    return False


if __name__ == "__main__":
    print("saved" if shot(sys.argv[1], float(sys.argv[2]) if len(sys.argv) > 2 else 0.5) else "no image")
