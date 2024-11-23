#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
This script is used to test the Google Test binaries in a given directory.
It uses the `subprocess` module to run the `--gtest_list_tests` command on each binary and checks the return code.
If the return code is 0, the binary is considered a Google Test binary and is added to the list of test binaries.

The script also includes a function to check if a given file is a Google Test binary.

Finally, the script provides a main function that takes a directory path as an argument and prints the list of test binaries.
"""

import os
import subprocess
import concurrent.futures
import json
import argparse
from datetime import datetime
from typing import List, Tuple, Any, Dict, Optional

import pytest
from rich.console import Console
from rich.table import Table
from rich.logging import RichHandler
import logging

# Initialize Rich console and logger
console = Console()
logging.basicConfig(
    level=logging.INFO, format="%(message)s", handlers=[RichHandler()]
)
logger = logging.getLogger("rich")


def is_test_binary(file_path: str) -> bool:
    """
    Checks if the given file is a Google Test binary by running `--gtest_list_tests`.

    Args:
        file_path (str): Path to the file to check.

    Returns:
        bool: True if the file is a Google Test binary, otherwise False.
    """
    try:
        result = subprocess.run(
            [file_path, '--gtest_list_tests'],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
            check=True
        )
        return result.returncode == 0
    except subprocess.CalledProcessError:
        return False
    except subprocess.TimeoutExpired:
        return False


def get_test_binaries(directory: str) -> List[str]:
    """
    Recursively searches the given directory for Google Test binaries.

    Args:
        directory (str): The directory to search.

    Returns:
        list: A list of paths to Google Test binaries.
    """
    test_binaries = []
    for root, _, files in os.walk(directory):
        for file in files:
            file_path = os.path.join(root, file)
            if os.access(file_path, os.X_OK) and is_test_binary(file_path):
                test_binaries.append(file_path)
    return test_binaries


def run_test_binary(test_binary: str, timeout: int) -> Tuple[str, int, int, List[str], List[Dict[str, Any]], datetime, datetime]:
    """
    Runs a Google Test binary and collects the test results.

    Args:
        test_binary (str): Path to the Google Test binary.
        timeout (int): Timeout for the test run in seconds.

    Returns:
        tuple: A tuple containing the test binary path, number of tests, number of failures,
               a list of failure reasons, detailed test results, start time, and end time.
    """
    start_time = datetime.now()
    try:
        result = subprocess.run(
            [test_binary, '--gtest_output=json'],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=timeout,
            check=True
        )
        end_time = datetime.now()
        output = result.stdout
        try:
            result_json = json.loads(output)
            num_tests = result_json.get('tests', 0)
            num_failures = result_json.get('failures', 0)
            failures = result_json.get('failures', [])
            failure_reasons = [failure['message'] for failure in failures]
            test_details = []
            for testsuite in result_json.get('testsuites', []):
                for testcase in testsuite.get('testsuite', []):
                    test_details.append({
                        "test": testcase['name'],
                        "status": testcase['result'],
                        "time": testcase['time'],
                        "failure_message": testcase.get('failure', {}).get('message', '') if testcase['result'] == 'FAILED' else ''
                    })
            return (test_binary, num_tests, num_failures, failure_reasons, test_details, start_time, end_time)
        except json.JSONDecodeError:
            return (test_binary, 0, 0, ["Failed to parse test output"], [], start_time, end_time)
    except subprocess.TimeoutExpired:
        end_time = datetime.now()
        return (test_binary, 0, 0, ["Timeout expired"], [], start_time, end_time)
    except subprocess.CalledProcessError as e:
        end_time = datetime.now()
        return (test_binary, 0, 0, [str(e)], [], start_time, end_time)


def write_summary(log: Any, results: List[Tuple[str, int, int, List[str], List[Dict[str, Any]], datetime, datetime]]) -> None:
    """
    Writes a summary of the test results to the log file.

    Args:
        log (file object): The log file to write to.
        results (list): A list of test results.
    """
    total_tests = sum(result[1] for result in results)
    total_failures = sum(result[2] for result in results)

    log.write("\nSummary:\n")
    log.write(f"  Total tests run: {total_tests}\n")
    log.write(f"  Total failures: {total_failures}\n")

    for binary, num_tests, num_failures, failure_reasons, test_details, start_time, end_time in results:
        log.write(f"Binary: {binary}\n")
        log.write(f"  Total tests: {num_tests}\n")
        log.write(f"  Total failures: {num_failures}\n")
        log.write(f"  Start time: {start_time}\n")
        log.write(f"  End time: {end_time}\n")
        log.write(f"  Duration: {end_time - start_time}\n")
        if failure_reasons:
            log.write("  Failure reasons:\n")
            for reason in failure_reasons:
                log.write(f"    - {reason}\n")
        for detail in test_details:
            status = detail["status"]
            log.write(f"  Test: {detail['test']}\n")
            log.write(f"    Status: {status}\n")
            log.write(f"    Time: {detail['time']}\n")
            if detail["failure_message"]:
                log.write(
                    f"    Failure message: {detail['failure_message']}\n")
        log.write("\n")

    log.write("Summary:\n")
    log.write(f"  Total tests run: {total_tests}\n")
    log.write(f"  Total failures: {total_failures}\n")


def pytest_write_summary(results: List[Tuple[str, int, int, List[str], List[Dict[str, Any]], datetime, datetime]]) -> Dict[str, Any]:
    """
    Writes a summary of the test results for pytest.

    Args:
        results (list): A list of test results.

    Returns:
        dict: A summary of the test results.
    """
    total_tests = sum(result[1] for result in results)
    total_failures = sum(result[2] for result in results)
    summary = {
        "total_tests": total_tests,
        "total_failures": total_failures,
        "details": []
    }

    for binary, num_tests, num_failures, failure_reasons, test_details, start_time, end_time in results:
        details = {
            "binary": binary,
            "total_tests": num_tests,
            "total_failures": num_failures,
            "start_time": start_time.isoformat(),
            "end_time": end_time.isoformat(),
            "duration": str(end_time - start_time),
            "failure_reasons": failure_reasons,
            "test_details": test_details
        }
        summary["details"].append(details)

    return summary


@pytest.fixture(scope="session")
def google_test_results(request: Any) -> Dict[str, Any]:
    """
    Pytest fixture to get Google Test results.

    Args:
        request (FixtureRequest): The fixture request object.

    Returns:
        dict: A summary of the test results.
    """
    directory = request.config.getoption("--gtest_dir")
    timeout = request.config.getoption("--gtest_timeout")
    max_workers = request.config.getoption("--gtest_max_workers")
    test_binaries = get_test_binaries(directory)

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
        future_to_binary = {executor.submit(
            run_test_binary, binary, timeout): binary for binary in test_binaries}
        for future in concurrent.futures.as_completed(future_to_binary):
            binary = future_to_binary[future]
            try:
                results.append(future.result())
            except Exception as exc:
                results.append(
                    (binary, 0, 0, [str(exc)], [],
                     datetime.now(), datetime.now())
                )

    return pytest_write_summary(results)


def pytest_addoption(parser: Any) -> None:
    """
    Adds custom command-line options to pytest.

    Args:
        parser (argparse.ArgumentParser): The argument parser object.
    """
    parser.addoption("--gtest_dir", action="store", default=".",
                     help="Directory to search for Google Test binaries")
    parser.addoption("--gtest_timeout", action="store", default=300,
                     type=int, help="Timeout for each test binary in seconds")
    parser.addoption("--gtest_max_workers", action="store", default=4,
                     type=int, help="Maximum number of parallel workers")


def pytest_configure(config: Any) -> None:
    """
    Configures pytest with custom settings.

    Args:
        config (Config): The pytest config object.
    """
    config._google_test_results = None


def pytest_sessionstart(session: Any) -> None:
    """
    Called after the pytest session has started.

    Args:
        session (Session): The pytest session object.
    """
    config = session.config
    config._google_test_results = session.config.pluginmanager.get_plugin(
        "google_test_results")


def pytest_terminal_summary(terminalreporter: Any, config: Any) -> None:
    """
    Writes a summary of the test results to the terminal.

    Args:
        terminalreporter (TerminalReporter): The terminal reporter object.
        config (Config): The pytest config object.
    """
    if config._google_test_results:
        results = config._google_test_results
        table = Table(title="Google Test Summary")
        table.add_column("Metric", style="cyan", no_wrap=True)
        table.add_column("Value", style="magenta")

        table.add_row("Total tests run", str(results["total_tests"]))
        table.add_row("Total failures", str(results["total_failures"]))

        terminalreporter.write(table)
        for detail in results["details"]:
            terminalreporter.write(f"Binary: {detail['binary']}\n")
            terminalreporter.write(f"  Total tests: {detail['total_tests']}\n")
            terminalreporter.write(
                f"  Total failures: {detail['total_failures']}\n")
            for reason in detail["failure_reasons"]:
                terminalreporter.write(f"    - {reason}\n")


def main(directory: str, log_file: str, max_workers: int, only_failed: bool, timeout: int, retries: int) -> None:
    """
    Main function to find, run, and log Google Test binaries.

    Args:
        directory (str): Directory to search for test binaries.
        log_file (str): Path to the log file.
        max_workers (int): Maximum number of parallel workers.
        only_failed (bool): Whether to only rerun failed tests.
        timeout (int): Timeout for each test binary in seconds.
        retries (int): Number of retries for failed tests.
    """
    test_binaries = get_test_binaries(directory)
    if not test_binaries:
        logger.warning(
            "No Google Test binaries found in the specified directory.")
        return

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
        future_to_binary = {executor.submit(
            run_test_binary, binary, timeout): binary for binary in test_binaries}
        for future in concurrent.futures.as_completed(future_to_binary):
            binary = future_to_binary[future]
            try:
                results.append(future.result())
                logger.info(f"Completed tests for binary: {binary}")
            except Exception as exc:
                results.append(
                    (binary, 0, 0, [str(exc)], [],
                     datetime.now(), datetime.now())
                )
                logger.error(f"Error running binary {binary}: {exc}")

    with open(log_file, 'w', encoding="utf-8") as log:
        write_summary(log, results)

    if only_failed:
        failed_tests = [result for result in results if result[2] > 0]
        if failed_tests:
            logger.info("\nRe-running failed tests...")
            for attempt in range(retries):
                logger.info(f"Retry attempt {attempt + 1}...")
                with concurrent.futures.ThreadPoolExecutor(max_workers=max_workers) as executor:
                    future_to_binary = {executor.submit(
                        run_test_binary, result[0], timeout): result[0] for result in failed_tests}
                    new_results = []
                    for future in concurrent.futures.as_completed(future_to_binary):
                        binary = future_to_binary[future]
                        try:
                            new_results.append(future.result())
                            logger.info(
                                f"Retry completed for binary: {binary}")
                        except Exception as exc:
                            new_results.append(
                                (binary, 0, 0, [str(exc)], [],
                                 datetime.now(), datetime.now())
                            )
                            logger.error(
                                f"Error re-running binary {binary}: {exc}")

                failed_tests = [
                    result for result in new_results if result[2] > 0]
                results.extend(new_results)
                if not failed_tests:
                    logger.info("All failed tests have passed on retry.")
                    break
            else:
                logger.warning("Some tests are still failing after retries.")

            with open(log_file, 'a', encoding="utf-8") as log:
                log.write("\nRe-run Summary:\n")
                write_summary(log, new_results)

    logger.info(f"\nResults have been written to {log_file}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Run all Google Test binaries in a directory and summarize results."
    )
    parser.add_argument(
        "directory", help="Directory to search for test binaries"
    )
    parser.add_argument(
        "--log_file",
        default=f"test_results_{datetime.now().strftime('%Y%m%d_%H%M%S')}.log",
        help="Log file to save test results"
    )
    parser.add_argument(
        "--max_workers",
        type=int,
        default=4,
        help="Maximum number of parallel workers"
    )
    parser.add_argument(
        "--only_failed",
        action='store_true',
        help="Only rerun failed tests"
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=300,
        help="Timeout for each test binary in seconds"
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=1,
        help="Number of retries for failed tests"
    )

    args = parser.parse_args()

    main(args.directory, args.log_file, args.max_workers,
         args.only_failed, args.timeout, args.retries)
