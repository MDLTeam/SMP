/// General Matrix Multiplication
#include <HElib/FHE.h>
#include <HElib/FHEContext.h>
#include <HElib/EncryptedArray.h>
#include <HElib/NumbTh.h>

#include "SMP/DoublePacking.hpp"
#include "SMP/Matrix.hpp"
#include "SMP/Timer.hpp"
#include <boost/asio.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <iostream>
#include <numeric>
#include <list>
using boost::asio::ip::tcp;
static void print_time(std::string const& doc, Duration_t const& dur) {
    std::cout << doc << " " << time_as_millsecond(dur) << std::endl;
}

inline long round_div(long a, long b) {
    return (a + b - 1) / b;
}

void zero(Matrix &mat) {
    for (long i = 0; i < mat.NumRows(); i++)
        for (long j = 0; j < mat.NumCols(); j++)
            mat[i][j] = 0;
}

void randomize(Matrix &mat) {
    for (long i = 0; i < mat.NumRows(); i++)
        for (long j = 0; j < mat.NumCols(); j++)
            mat[i][j] = NTL::RandomBnd(2L);
}

void mod_to_plain(Matrix &mat, long prime) {
    for (long i = 0; i < mat.NumRows(); i++)
        for (long j = 0; j < mat.NumCols(); j++)
            mat[i][j] %= prime;
}

struct Duplication {
    long blk_id;
    long dup;
    long slots_left;
};

Duplication compute_duplications(const long total_rows, 
                                 const long x_blk_id, 
                                 const long num_slots) {
    long N = std::min((x_blk_id  + 1) * num_slots, total_rows) - x_blk_id * num_slots;
    Duplication d;
    d.blk_id = x_blk_id;
    d.dup = num_slots / N;
    d.slots_left = num_slots - d.dup * N;
    return d;
}

std::vector<long> compute_rotations(long x_blk, long total_rows_X,
                                    long y_blk, long total_rows_Y,
                                    long num_slots) {
    auto dupA = compute_duplications(total_rows_X, x_blk, num_slots);
    auto dupB = compute_duplications(total_rows_Y, y_blk, num_slots);
    std::vector<long> rotations;
    long sze;
    // fully packed
    if (dupA.dup == 1 and dupB.dup == 1) {
        sze = num_slots;
    } else if (dupA.slots_left != 0 or dupB.slots_left != 0) {
        // cumble to implement, so just skip this case
        std::cout << "cumble\n" << std::endl;
        sze = num_slots;
    } else {
        long NA = (num_slots - dupA.slots_left) / dupA.dup;
        long NB = (num_slots - dupB.slots_left) / dupB.dup;
        sze = std::max(NA, NB) / (dupA.dup * dupB.dup);
        sze = std::max(1L, sze);
    }
    rotations.resize(sze);
    std::iota(rotations.begin(), rotations.end(), 0);
    return rotations;
}

void fill_compute(Matrix& mat, 
                  int x, int y, int k,
                  const std::vector<NTL::zz_pX> &slots,
                  const EncryptedArray *ea) {
    const long l = ea->size();
    const long d = ea->getDegree();
    long row_start = x * l;
    long row_end = row_start + l;
    long col_start = y * l;
    long col_end = col_start + l;

    assert(slots.size() == l);
    for (long ll = 0; ll < l; ll++) {
        long computed = NTL::coeff(slots[ll], d - 1)._zz_p__rep;
        long row = row_start + ll;
        long col = col_start + ll + k;
        if (col >= col_end)
            col = col_start + col % l;
        mat.put(row, col, computed);
    }
}

long ceil_round(long a, long l) {
    return (a + l - 1) / l * l;
}

FHEcontext receive_context(std::istream &s) {
    unsigned long m, p, r;
    std::vector<long> gens, ords;
    readContextBase(s, m, p, r, gens, ords);
    FHEcontext context(m, p, r, gens, ords);
    s >> context;
    return context;
}

void send_context(std::ostream &s, FHEcontext const& context) {
    writeContextBase(s, context);
    s << context;
}

void play_client(tcp::iostream &conn, 
                 FHESecKey &sk, 
                 FHEcontext &context,
                 const long n1,
                 const long n2,
                 const long n3) {
    FHEPubKey ek(sk);
    ek.makeSymmetric();
    conn << ek;
    const EncryptedArray *ea = context.ea;
    const long l = ea->size();
    const long d = ea->getDegree();

    NTL::SetSeed(NTL::to_ZZ(123));
    Matrix A, B, ground_truth;
    A.SetDims(n1, n2);
    B.SetDims(n2, n3);
    randomize(A);
    randomize(B);
    ground_truth = mul(A, B);
    mod_to_plain(ground_truth, context.alMod.getPPowR());
    /// print grouth truth for debugging
    // save_matrix(std::cout, ground_truth);
    const long MAX_X1 = round_div(A.NumRows(), l);
    const long MAX_Y1 = round_div(A.NumCols(), d);
    const long MAX_X2 = round_div(B.NumCols(), l);

    std::vector<std::vector<Ctxt>> uploading; 
    uploading.resize(MAX_X1, std::vector<Ctxt>(MAX_Y1, sk));
    auto start_time = Clock::now();
    /// encrypt matrix 
    for (int x = 0; x < MAX_X1; x++) {
        for (int k = 0; k < MAX_Y1; k++) {
            internal::BlockId blk = {x, k};
            auto packed_rows = internal::partition(A, blk, *ea, false);
            //ea->skEncrypt(uploading[x][k], sk, packed_rows.polys);
        }
    }
    auto end_time = Clock::now();
    print_time("encryption", end_time - start_time);

    /// send ciphertexts of matrix 
    start_time = Clock::now();
    for (auto const& row : uploading) {
        for (auto const& ctx : row)
            conn << ctx;
    }
    std::cout << MAX_X1 * MAX_Y1 << " ciphertexts sent" << std::endl;
    end_time = Clock::now();
    print_time("client->server", end_time - start_time);

    /// waiting results
    long rows_of_A = A.NumRows();
    long rows_of_Bt = B.NumCols(); // Bt::Rows = B::Cols
    std::list<Ctxt> ret_ctxs;
    for (int x = 0; x < MAX_X1; x++) {
        for (int y = 0; y < MAX_X2; y++) {
            size_t num_rots = compute_rotations(x, rows_of_A, 
                                                y, rows_of_Bt, l).size();
            for (size_t k = 0; k < num_rots; k++) {
                Ctxt result(sk);
                conn >> result;
                ret_ctxs.emplace_back(result);
            }
        }
    }
    /// decrypt
    Matrix computed;
    computed.SetDims(A.NumRows(), B.NumCols());
    zero(computed);
    int x = 0;
    int y = 0;
    auto itr = ret_ctxs.begin();
    std::vector<NTL::zz_pX> slots;
    start_time = Clock::now();
    for (int x = 0; x < MAX_X1; x++) {
        for (int y = 0; y < MAX_X2; y++) {
            size_t num_rots  = compute_rotations(x, rows_of_A, 
                                                 y, rows_of_Bt, l).size();
            for (size_t k = 0; k < num_rots; k++) {
                //ea->decrypt(*itr++, sk, slots); 
                fill_compute(computed, x, y, k, slots, ea);
            }
        }
    }
    end_time = Clock::now();
    print_time("decryption", end_time - start_time);
    if (!::is_same(ground_truth, computed)) 
        std::cerr << "The computation seems wrong " << std::endl;
    else 
        std::cout << "passed" << std::endl;
}

void my_encode(EncryptedArray const& ea, 
               zzX &ret,
               std::vector<NTL::zz_pX> &slots) {
    auto const& encoder = ea.getContext().alMod.getDerived(PA_zz_p());
    NTL::zz_pX tmp;
    encoder.CRT_reconstruct(tmp, slots);
    // NTL::conv(ret, tmp);
}

void play_server(tcp::iostream &conn, 
                 const long n1,
                 const long n2,
                 const long n3) {
    auto context = receive_context(conn);
    const EncryptedArray *ea = context.ea;
    const long l = ea->size();
    const long d = ea->getDegree();
    FHEPubKey ek(context);
    conn >> ek;
    NTL::SetSeed(NTL::to_ZZ(123));

    Matrix A, B;
    A.SetDims(n1, n2);
    B.SetDims(n2, n3);
    randomize(A);
    randomize(B);

    const long MAX_X1 = round_div(A.NumRows(), l);
    const long MAX_Y1 = round_div(A.NumCols(), d);
    Matrix Bt;
    transpose(&Bt, B);
    const long MAX_X2 = round_div(Bt.NumRows(), l);
    const long MAX_Y2 = round_div(Bt.NumCols(), d);
    auto start_time = Clock::now();
    std::vector<std::vector<NewPlaintextArray>> precomputed;
    precomputed.resize(MAX_X2, std::vector<NewPlaintextArray>(MAX_Y1, *ea));
    for (int y = 0; y < MAX_X2; y++) {
        for (int k = 0; k < MAX_Y1; k++) {
            internal::BlockId blk = {y, k};
            auto packed_rows = internal::partition(Bt, blk, *ea, true);
            encode(*ea, precomputed[y][k], packed_rows.polys);
        }
    }
    auto end_time = Clock::now();
    print_time("precomputation", end_time - start_time);

    /// receving ciphertexts from the client 
    std::vector<std::vector<Ctxt>> received; 
    received.resize(MAX_X1, std::vector<Ctxt>(MAX_Y1, ek));
    for (int x = 0; x < MAX_X1; x++) {
        for (int k = 0; k < MAX_Y1; k++)
            conn >> received[x][k];
    }

    /// compute the matrix mulitplication
    long rows_of_A = A.NumRows();
    long rows_of_Bt = Bt.NumRows();
    size_t send_ctx = 0;
    zzX packed_polys;
    Duration_t computation(0), network(0);
    for (int x = 0; x < MAX_X1; x++) {
        for (int y = 0; y < MAX_X2; y++) {
            start_time = Clock::now();
            std::vector<long> rotations = compute_rotations(x, rows_of_A, 
                                                            y, rows_of_Bt, l);
            size_t num_rotations = rotations.size();
            std::vector<Ctxt> summations(num_rotations, ek);
            for (int k = 0; k < MAX_Y1; k++) {
                for (size_t rot = 0; rot < num_rotations; rot++) {
                    auto rotated(precomputed[y][k]);
                    rotate(*ea, rotated, -rotations[rot]);
                    /// pack the polys into one poly
                    //my_encode(*ea, packed_polys, rotated.getData());
                    Ctxt client_ctx(received[x][k]);
                    /// ctx * plain
                    client_ctx.multByConstant(packed_polys);
                    summations[rot] += client_ctx;
                }
            }
            for (auto &sm : summations) 
                sm.modDownToLevel(1);
            end_time = Clock::now();
            computation += (end_time - start_time);

            start_time = Clock::now();
            for (auto const&sm : summations) 
                conn << sm;
            end_time = Clock::now();
            network += (end_time - start_time);
            send_ctx += summations.size();
        }
    }
    print_time("computation", computation);
    print_time("server->client", network);
    std::cout << "Sent " << send_ctx << " ciphertexts" << std::endl;
}

int run_client(long n1, long n2, long n3) {
    tcp::iostream conn("127.0.0.1", "12345");
    if (!conn) {
        std::cerr << "Can not connect to server!" << std::endl;
        return -1;
    }
    const long m = 8192;
    //const long p = 641;
    const long p = 3329;
    const long r = 1;
    const long L = 3;
    FHEcontext context(m, p, r);
    context.bitsPerLevel = 14 + std::ceil(std::log(m)/2 + r* std::log(p));
    buildModChain(context, L);
    std::cout << "kappa = " << context.securityLevel() << std::endl;
    FHESecKey sk(context);
    sk.GenSecKey(64);
    /// send FHEcontext obj
    send_context(conn, context);
    /// send the evaluation key
    play_client(conn, sk, context, n1, n2, n3);
    conn.close();
    return 1;
}

int run_server(long n1, long n2, long n3) {
    boost::asio::io_service ios;
    tcp::endpoint endpoint(tcp::v4(), 12345);
    tcp::acceptor acceptor(ios, endpoint);

    for (;;) {
        tcp::iostream conn;
        boost::system::error_code err;
        acceptor.accept(*conn.rdbuf(), err);
        if (!err) {
            std::cout << "Connected!" << std::endl;
            play_server(conn, n1, n2, n3);
            break;
        }
    }  
}

int main(int argc, char *argv[]) {
    ArgMapping argmap;
    long role;
    long n1 = 8; 
    long n2 = 8; 
    long n3 = 8;
    argmap.arg("N", n1, "n1");
    argmap.arg("M", n2, "n2");
    argmap.arg("D", n3, "n3");
    argmap.arg("R", role, "role");
    argmap.parse(argc, argv);
    if (role == 0) {
        return run_server(n1, n2, n3);
    } else if (role == 1) {
        return run_client(n1, n2, n3);
    }
}
