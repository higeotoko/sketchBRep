"""Optional interoperability test: pip install -r tests/requirements-chatgpt-sdk.txt.

Run with the built sbr_chatgpt_transport_tests executable as the only argument.
The harness owns a temporary model and never opens a user's project or local pipe.
"""
import asyncio
import importlib.metadata
import json
import subprocess
import sys
from urllib.parse import urlsplit

import httpx
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client


async def check(url):
    async with httpx.AsyncClient(trust_env=False, timeout=15) as http:
        async with streamable_http_client(url, http_client=http) as (read, write, _):
            async with ClientSession(read, write) as session:
                initialized = await session.initialize()
                tools = await session.list_tools()
                assert any(tool.name == "create_box" for tool in tools.tools)
                created = await session.call_tool(
                    "create_box", {"origin": [0, 0, 0], "width": 10, "depth": 20, "height": 30}
                )
                assert not created.isError
                objects = await session.call_tool("list_objects", {})
                assert len(json.loads(objects.content[0].text)["objects"]) == 1
                undone = await session.call_tool("undo", {})
                assert not undone.isError and json.loads(undone.content[0].text)["undone"]
                after = await session.call_tool("list_objects", {})
                assert json.loads(after.content[0].text)["objects"] == []
                print(
                    "Official Python MCP SDK", importlib.metadata.version("mcp"),
                    "negotiated", initialized.protocolVersion, "tools", len(tools.tools),
                    "create/read/undo passed",
                )


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: python tests/chatgpt_sdk_smoke.py <sbr_chatgpt_transport_tests executable>")
    process = subprocess.Popen([sys.argv[1], "--sdk-smoke"], stdout=subprocess.PIPE, text=True)
    try:
        url = process.stdout.readline().strip()
        parsed = urlsplit(url)
        if parsed.scheme != "http" or parsed.hostname != "127.0.0.1":
            raise RuntimeError("The test harness did not provide a loopback endpoint")
        asyncio.run(asyncio.wait_for(check(url), 30))
    finally:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
