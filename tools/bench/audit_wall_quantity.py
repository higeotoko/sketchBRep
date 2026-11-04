"""Compare supplied wall-quantity inputs with independent Decimal arithmetic.

Synthetic inputs only. Does not check geometry, load derivation or compliance.
Run with the built CLI and its required shared libraries available.
"""
import argparse
import json
from decimal import Decimal, localcontext
from pathlib import Path
import subprocess


def audit(executable):
    process = subprocess.Popen(
        [str(Path(executable).resolve())], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        text=True, encoding="utf-8",
    )
    rows = []
    try:
        for height in [3, 6, 12]:
            for share in ["0.1", "0.5", "1"]:
                for designated in [False, True]:
                    for wind in [50, 75]:
                        total = 250
                        floors = [
                            {"id": "1", "area_m2": 50, "weight_kn": total,
                             "wind_x_m2": 30, "wind_y_m2": 15, "walls_complete": True},
                            {"id": "2", "area_m2": 25, "weight_kn": float(Decimal(share) * total),
                             "wind_x_m2": 12, "wind_y_m2": 6, "walls_complete": True},
                        ]
                        inputs = {"height_m": height, "co_designated": designated,
                                  "wind_coefficient_cm_m2": wind,
                                  "input_basis": "Synthetic numerical audit only",
                                  "floors": floors, "walls": []}
                        request = {"command": "check_wall_quantity", "args": {"inputs": inputs}}
                        process.stdin.write(json.dumps(request) + "\n")
                        process.stdin.flush()
                        response = json.loads(process.stdout.readline())
                        if not response.get("ok"):
                            raise ValueError(response)
                        errors = []
                        with localcontext() as context:
                            context.prec = 50
                            for actual in response["result"]["rows"]:
                                floor = int(actual["floor"]) - 1
                                alpha = Decimal(1) if floor == 0 else Decimal(share)
                                period = Decimal("0.03") * height
                                ai = 1 + (1 / alpha.sqrt() - alpha) * 2 * period / (1 + 3 * period)
                                co = Decimal("0.3" if designated else "0.2")
                                weight = Decimal(total) if floor == 0 else Decimal(share) * total
                                earthquake = ai * co * weight / Decimal("1.96")
                                key = "wind_x_m2" if actual["direction"] == "X" else "wind_y_m2"
                                wind_m = Decimal(str(floors[floor][key])) * wind / 100
                                expected = {"ai": ai, "earthquake_m": earthquake,
                                            "wind_m": wind_m, "required_m": max(earthquake, wind_m)}
                                for field, value in expected.items():
                                    error = abs(Decimal(str(actual[field])) - value)
                                    if error >= Decimal("1e-10"):
                                        raise ValueError((field, actual[field], str(value)))
                                    errors.append(float(error))
                        rows.append({"height_m": height, "upper_weight_fraction": share,
                                     "co_designated": designated, "wind_coefficient": wind,
                                     "rows": 4, "max_abs_error": max(errors), "passed": True})
    finally:
        process.stdin.close()
        try:
            exit_code = process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise
    if exit_code:
        raise RuntimeError(f"CLI exited with {exit_code}")
    return {"scope": "Independent Decimal precision-50 arithmetic; synthetic supplied inputs only",
            "cases": rows, "case_count": len(rows), "floor_direction_rows": 4 * len(rows),
            "max_abs_error": max(row["max_abs_error"] for row in rows)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = audit(args.cli)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: value for key, value in result.items() if key != "cases"}))
