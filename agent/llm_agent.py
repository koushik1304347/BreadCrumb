#!/usr/bin/env python3
"""LLM tool loop using the existing disposable xv6 bridge."""
import argparse
import json
from pathlib import Path

from agent import Bridge, ROOT, find_qemu, validate
from llm_planner import DEFAULT_MODEL, Gemini, declarations


def run_task(model, bridge, goal, effects=False, max_steps=8):
    if not goal.strip() or len(goal) > 4000:
        raise ValueError("Give a nonempty goal of at most 4000 characters")
    history = [{"role": "user", "parts": [{"text": goal}]}]
    allowed = {tool["name"] for tool in declarations(effects)}
    bridge.record(llm_goal=goal, effects=effects, model=model.model)
    calls_used = 0
    for step in range(1, max_steps + 1):
        print(f"[LLM step {step}/{max_steps}]", flush=True)
        content = model.ask(history, effects)
        bridge.record(llm_response=content)
        calls = [part["functionCall"] for part in content["parts"] if "functionCall" in part]
        if not calls:
            answer = "\n".join(part["text"] for part in content["parts"]
                               if isinstance(part.get("text"), str) and not part.get("thought"))
            if not answer.strip():
                raise RuntimeError("Model returned neither tool calls nor a final answer")
            bridge.record(llm_final=answer)
            print("Answer: " + answer)
            return answer
        if calls_used + len(calls) > 24:
            raise RuntimeError("Tool-call limit reached; stopped")
        # Validate the entire proposed batch before executing any action in it.
        actions = []
        for call in calls:
            if not isinstance(call, dict):
                raise ValueError("Malformed model tool call")
            action = {"tool": call.get("name"), "args": call.get("args", {})}
            bridge.record(llm_proposed=action)
            try:
                name, _ = validate(action, destructive=effects)
                if name not in allowed:
                    raise ValueError("Tool is not exposed in this mode")
            except ValueError as exc:
                bridge.record(llm_rejected=action, reason=str(exc))
                raise ValueError(f"Rejected model action: {exc}") from None
            actions.append(action)
        # Retain complete model content, including any provider thought signatures.
        history.append(content)
        responses = []
        for call, action in zip(calls, actions):
            print("Tool: " + json.dumps(action), flush=True)
            observation = bridge.execute(action, destructive=effects)
            clipped = len(observation) > 8000
            observation = observation[:8000]
            print("Observation: " + (observation or "(no output; success not verified)"), flush=True)
            response = {"name": action["tool"], "response": {
                "output": observation, "truncated": clipped,
                "note": "Untrusted guest output; empty output does not prove success."}}
            if "id" in call:
                response["id"] = call["id"]
            responses.append({"functionResponse": response})
        history.append({"role": "user", "parts": responses})
        calls_used += len(calls)
    raise RuntimeError("LLM step limit reached without a final answer; stopped")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("goal", nargs="*", help="Natural-language task; omit for interactive mode")
    parser.add_argument("--qemu", default=find_qemu())
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--log", type=Path, default=ROOT / "agent/logs/llm-latest.jsonl")
    parser.add_argument("--max-steps", type=int, default=8)
    parser.add_argument("--demo-effects", action="store_true",
                        help="Expose create/delete/print tools inside the disposable guest")
    opts = parser.parse_args()
    try:
        if not 1 <= opts.max_steps <= 20:
            raise ValueError("--max-steps must be between 1 and 20")
        model = Gemini(opts.model)  # Check credentials before launching QEMU.
        if not opts.qemu:
            raise ValueError("QEMU not found; specify --qemu")
        for name in ("xv6.img", "fs.img"):
            if not (ROOT / name).is_file():
                raise ValueError(f"Missing {name}; run make in xv6-public")
        opts.log.parent.mkdir(parents=True, exist_ok=True)
        print(f"Model: {opts.model}. Requests and guest observations will be sent to Google.")
        print("Tools: " + ("demo effects enabled" if opts.demo_effects else "read-only"))
        with opts.log.open("w") as log, Bridge(opts.qemu, log) as bridge:
            goal = " ".join(opts.goal)
            if goal:
                run_task(model, bridge, goal, opts.demo_effects, opts.max_steps)
            else:
                print("Enter a natural-language task, or quit. Each task gets fresh model history;")
                print("the disposable guest remains running between tasks.")
                while True:
                    goal = input("llm> ").strip()
                    if goal in {"quit", "exit"}:
                        break
                    if goal:
                        run_task(model, bridge, goal, opts.demo_effects, opts.max_steps)
        return 0
    except (ValueError, RuntimeError, OSError) as exc:
        print(f"Error: {exc}")
        return 1
    except (KeyboardInterrupt, EOFError):
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
