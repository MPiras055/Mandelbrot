import subprocess
import json
import csv
import statistics
import argparse
import sys
import os

# ==========================================
# CONFIGURATION CONSTANTS
# ==========================================
# Define the build directory relative to where this Python script is located
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(SCRIPT_DIR,"..","build")

# Define the executable path
TARGET_EXE = os.path.join(BUILD_DIR, "benchmarkUtil")

# Define the benchmark parameters
THREAD_COUNTS = [1, 2, 4, 8, 12, 16, 20 ,24, 30, 32, 38, 42, 48, 52, 56,64]
ZOOM_LEVELS = [1e10, 1e80]
# ==========================================

def run_benchmark(exe_path, threads, width, height, zoom, runs):
    times = []
    
    for i in range(runs):
        cmd = [
            exe_path,
            '-t', str(threads),
            '-w', str(width),
            '-h', str(height),
            '-z', str(zoom)
        ]
        
        print(f"    Run {i+1}/{runs}...", end='', flush=True)
        
        try:
            # capture_output=True captures stdout and stderr separately
            result = subprocess.run(cmd, capture_output=True, text=True, check=True)
            
            # Parse only standard output (where our JSON lives)
            data = json.loads(result.stdout.strip())
            time_ms = data['time_ms']
            times.append(time_ms)
            
            print(f" {time_ms} ms")
            
        except subprocess.CalledProcessError as e:
            print(f"\n[Error] The C++ executable failed with exit code {e.returncode}")
            print(f"Command: {' '.join(cmd)}")
            print(f"Stderr:\n{e.stderr}")
            sys.exit(1)
        except json.JSONDecodeError:
            print(f"\n[Error] Failed to parse JSON from executable.")
            print(f"Stdout was: {result.stdout}")
            sys.exit(1)
            
    mean_time = statistics.mean(times)
    # stddev requires at least 2 data points
    std_dev = statistics.stdev(times) if runs > 1 else 0.0
    
    return mean_time, std_dev

def main():
    # We still use argparse for dimensions and runs, so you can tweak them easily
    parser = argparse.ArgumentParser(description="Automated Scalability Benchmark for MandelbrotEngine")
    parser.add_argument('--runs', type=int, default=5, help="Number of times to run each configuration")
    parser.add_argument('--width', type=int, default=3840, help="Maximum render width")
    parser.add_argument('--height', type=int, default=2160, help="Maximum render height")
    
    args = parser.parse_args()

    if not os.path.isfile(TARGET_EXE):
        print(f"[Error] Executable not found at path: {TARGET_EXE}")
        print("Please ensure you have compiled the CMake project into the 'build' directory.")
        sys.exit(1)

    print(f"Starting Benchmark Suite")
    print(f"Executable:        {TARGET_EXE}")
    print(f"Target Resolution: {args.width}x{args.height}")
    print(f"Runs per config:   {args.runs}")
    print(f"Thread configs:    {THREAD_COUNTS}\n")

    for zoom in ZOOM_LEVELS:
        # Categorize the filename based on the zoom threshold from the C++ code
        zoom_label = "deep" if zoom > 100000.0 else "shallow"
        
        # New requested naming scheme
        csv_filename = f"result_zoom_{zoom_label}.csv"
        
        print(f"=== Testing {zoom_label.upper()} Zoom (z = {zoom}) ===")
        
        results = []
        for t in THREAD_COUNTS:
            print(f"  -> Testing with {t} threads:")
            mean_t, std_t = run_benchmark(TARGET_EXE, t, args.width, args.height, zoom, args.runs)
            print(f"  => Mean: {mean_t:.2f} ms | StdDev: {std_t:.2f} ms\n")
            results.append((t, mean_t, std_t))
            
        # Write results to CSV
        with open(csv_filename, mode='w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['Threads', 'Mean Time (ms)', 'Std Dev (ms)'])
            for t, m, s in results:
                writer.writerow([t, f"{m:.2f}", f"{s:.2f}"])
                
        print(f"Saved results to {csv_filename}\n")

if __name__ == "__main__":
    main()