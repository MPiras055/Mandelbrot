#include <iostream>
#include <vector>
#include <complex>
#include <cmath>
#include <chrono>
#include <random>
#include <thread>
#include <algorithm>

using namespace std;

typedef complex<double> dcmplx;

// Function to compute the validity radius for a single point
double computeRadius(dcmplx c0, int max_iter, double epsilon) {
    dcmplx z(0, 0);
    dcmplx a(0, 0);
    dcmplx b(0, 0);
    dcmplx c(0, 0);
    dcmplx d(0, 0);

    for (int i = 0; i < max_iter; ++i) {
        // Calculate new values using current state
        // (Must evaluate before updating z, a, b, c)
        dcmplx d_new = 2.0 * z * d + 2.0 * a * c + b * b;
        dcmplx c_new = 2.0 * z * c + 2.0 * a * b;
        dcmplx b_new = 2.0 * z * b + a * a;
        dcmplx a_new = 2.0 * z * a + 1.0;
        dcmplx z_new = z * z + c0;

        // Apply updates
        d = d_new;
        c = c_new;
        b = b_new;
        a = a_new;
        z = z_new;

        // Standard Mandelbrot escape condition (optional for radius calc, 
        // but realistic for actual reference orbit generation)
        if (norm(z) > 4.0) break; 
    }

    // Protect against division by zero at iteration 0
    double abs_d = abs(d);
    if (abs_d == 0.0) return 999.0; // Arbitrary large safe radius

    // R = (epsilon / |D|)^(1/4)
    return pow(epsilon / abs_d, 0.25);
}

// Worker function for threads
void computeChunk(const vector<dcmplx>& points, vector<double>& radii, 
                  int start_idx, int end_idx, int max_iter, double epsilon) {
    for (int i = start_idx; i < end_idx; ++i) {
        radii[i] = computeRadius(points[i], max_iter, epsilon);
    }
}

int main() {
    // Benchmark Settings
    const int N = 10000;         // Number of random points (references) to compute
    const int MAX_ITER = 1000;   // Iterations per point
    const double EPSILON = 1e-9; // Pixel tolerance for the radius

    cout << "Generating " << N << " random reference points..." << endl;
    vector<dcmplx> points(N);
    vector<double> radii_seq(N);
    vector<double> radii_par(N);

    // Random number generation (focusing roughly on the Mandelbrot domain)
    mt19937 gen(42); 
    uniform_real_distribution<double> real_dist(-2.0, 0.5);
    uniform_real_distribution<double> imag_dist(-1.2, 1.2);

    for (int i = 0; i < N; ++i) {
        points[i] = dcmplx(real_dist(gen), imag_dist(gen));
    }

    // ==========================================
    // 1. Single-threaded Benchmark
    // ==========================================
    cout << "\nStarting Single-threaded computation..." << endl;
    auto start_seq = chrono::high_resolution_clock::now();
    
    for (int i = 0; i < N; ++i) {
        radii_seq[i] = computeRadius(points[i], MAX_ITER, EPSILON);
    }
    
    auto end_seq = chrono::high_resolution_clock::now();
    chrono::duration<double> diff_seq = end_seq - start_seq;
    cout << "Single-threaded time: " << diff_seq.count() << " seconds" << endl;

    // ==========================================
    // 2. Multi-threaded Benchmark
    // ==========================================
    cout << "\nStarting Multi-threaded computation..." << endl;
    
    // Determine number of hardware threads available
    unsigned int num_threads = thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4; // Fallback
    cout << "Using " << num_threads << " concurrent threads." << endl;

    auto start_par = chrono::high_resolution_clock::now();
    
    vector<thread> threads;
    int chunk_size = N / num_threads;
    
    for (unsigned int t = 0; t < num_threads; ++t) {
        int start_idx = t * chunk_size;
        // Last thread takes any remaining elements
        int end_idx = (t == num_threads - 1) ? N : start_idx + chunk_size;
        
        threads.push_back(thread(computeChunk, cref(points), ref(radii_par), 
                                 start_idx, end_idx, MAX_ITER, EPSILON));
    }

    // Join all threads
    for (auto& th : threads) {
        th.join();
    }
    
    auto end_par = chrono::high_resolution_clock::now();
    chrono::duration<double> diff_par = end_par - start_par;
    cout << "Multi-threaded time:  " << diff_par.count() << " seconds" << endl;

    // ==========================================
    // Results
    // ==========================================
    cout << "\nSpeedup Factor: " << diff_seq.count() / diff_par.count() << "x" << endl;

    return 0;
}