"""Tiny BeamNG MCP client: python mcp.py <tool> '<json args>'  or  python mcp.py lua '<code>'"""
import json, sys, urllib.request
def call(name, args):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": args}}).encode()
    req = urllib.request.Request("http://127.0.0.1:29292/mcp", body, {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"})
    txt = urllib.request.urlopen(req, timeout=60).read().decode()
    if not txt.startswith("{"):
        txt = [l[5:] for l in txt.splitlines() if l.startswith("data:")][-1]
    return json.loads(txt)
if __name__ == "__main__":
    if sys.argv[1] == "lua":
        r = call("run_lua", {"code": sys.argv[2]})
    else:
        r = call(sys.argv[1], json.loads(sys.argv[2]) if len(sys.argv) > 2 else {})
    for c in r.get("result", {}).get("content", []):
        print(c.get("text", c.get("type")))
    if "error" in r: print(r["error"])
