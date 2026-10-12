// Python bindings for lobkit (module lobkit._core).
//
// RULES THAT HOLD THROUGHOUT
//  * The C++ engine, replay and simulator are exception-free. Exceptions are created
//    here, at the boundary, and only here: bad configuration (ValueError), unreadable
//    files (OSError), and malformed input files (ParseError, a ValueError subclass that
//    carries the parser status and line). Engine rejects are DATA (Reject events), not
//    exceptions.
//  * Arrays are numpy structured arrays. Where an array is a view of C++ memory it is
//    read-only, and numpy holds a reference to the owner (a SimResult, or a capsule
//    that frees the buffer), so the memory lives as long as any view of it does. Each
//    function below states which of its arrays are views and which are copies.
//  * Long running calls release the GIL; they touch no Python object while it is released.

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "lobkit/book/engine.hpp"
#include "lobkit/book/invariants.hpp"
#include "lobkit/book/names.hpp"
#include "lobkit/md/lobster.hpp"
#include "lobkit/md/mapped_file.hpp"
#include "lobkit/md/replay.hpp"
#include "lobkit/sim/avellaneda_stoikov.hpp"
#include "lobkit/sim/report.hpp"
#include "lobkit/sim/simulator.hpp"
#include "lobkit/sim/synthetic.hpp"

namespace py = pybind11;
using namespace lobkit;

namespace {

// ---- dtypes -------------------------------------------------------------------------------

py::dtype make_dtype(
    std::initializer_list<std::tuple<const char*, const char*, std::size_t>> fields,
    std::size_t itemsize) {
  py::list names, formats, offsets;
  for (const auto& [name, fmt, off] : fields) {
    names.append(name);
    formats.append(fmt);
    offsets.append(off);
  }
  py::dict d;
  d["names"] = names;
  d["formats"] = formats;
  d["offsets"] = offsets;
  d["itemsize"] = itemsize;
  return py::dtype::from_args(d);
}

#define OFF(T, m) offsetof(T, m)

// Layout identical to lobkit::Event (48 bytes), so a vector of events is a numpy array.
py::dtype event_dtype() {
  static_assert(sizeof(Event) == 48);
  return make_dtype({{"seq", "u8", OFF(Event, seq)},
                     {"ts", "u8", OFF(Event, ts)},
                     {"id", "u8", OFF(Event, id)},
                     {"other", "u8", OFF(Event, other)},
                     {"price", "i8", OFF(Event, price)},
                     {"qty", "u4", OFF(Event, qty)},
                     {"type", "u1", OFF(Event, type)},
                     {"reason", "u1", OFF(Event, reason)},
                     {"side", "u1", OFF(Event, side)},
                     {"flags", "u1", OFF(Event, flags)}},
                    sizeof(Event));
}

py::dtype feed_dtype() {
  using md::FeedMsg;
  return make_dtype({{"ts", "u8", OFF(FeedMsg, ts)},
                     {"id", "u8", OFF(FeedMsg, id)},
                     {"new_id", "u8", OFF(FeedMsg, new_id)},
                     {"price", "i8", OFF(FeedMsg, price)},
                     {"qty", "u4", OFF(FeedMsg, qty)},
                     {"side", "u1", OFF(FeedMsg, side)},
                     {"type", "u1", OFF(FeedMsg, type)},
                     {"aux", "u1", OFF(FeedMsg, aux)}},
                    sizeof(FeedMsg));
}

py::dtype fill_dtype() {
  using sim::FillRecord;
  return make_dtype({{"ts", "u8", OFF(FillRecord, ts)},
                     {"id", "u8", OFF(FillRecord, id)},
                     {"side", "u1", OFF(FillRecord, side)},
                     {"price", "i8", OFF(FillRecord, price)},
                     {"qty", "u4", OFF(FillRecord, qty)},
                     {"maker", "?", OFF(FillRecord, maker)},
                     {"fee", "i8", OFF(FillRecord, fee_units)},
                     {"mid2_ref", "i8", OFF(FillRecord, mid2_ref)},
                     {"inventory_after", "i8", OFF(FillRecord, inventory_after)}},
                    sizeof(FillRecord));
}

// The simulator PathSample holds 128 bit integers, which numpy cannot represent, so the
// binding converts it once into this plain struct (checked for overflow).
struct PathRow {
  std::uint64_t ts;
  std::int64_t inventory, not_reported, spread2, inventory2, fees2, total2, mid2;
};
py::dtype path_dtype() {
  return make_dtype({{"ts", "u8", OFF(PathRow, ts)},
                     {"inventory", "i8", OFF(PathRow, inventory)},
                     {"not_reported", "i8", OFF(PathRow, not_reported)},
                     {"spread2", "i8", OFF(PathRow, spread2)},
                     {"inventory2", "i8", OFF(PathRow, inventory2)},
                     {"fees2", "i8", OFF(PathRow, fees2)},
                     {"total2", "i8", OFF(PathRow, total2)},
                     {"mid2", "i8", OFF(PathRow, mid2)}},
                    sizeof(PathRow));
}

// ---- array helpers --------------------------------------------------------------------------

void make_readonly(py::array& a) {
  a.attr("setflags")(py::arg("write") = false);
}

// A read-only view of `n` elements at `data`, kept alive by `owner`.
py::array view_of(const py::dtype& dt, const void* data, std::size_t n, std::size_t itemsize,
                  py::handle owner) {
  py::array a(dt, {static_cast<py::ssize_t>(n)}, {static_cast<py::ssize_t>(itemsize)}, data, owner);
  make_readonly(a);
  return a;
}

// Takes ownership of a vector and returns a read-only array over its buffer. The vector is
// freed when the last array that views it is garbage collected.
template <class T>
py::array adopt(std::vector<T>&& v, const py::dtype& dt) {
  auto* heap = new std::vector<T>(std::move(v));
  py::capsule owner(heap, [](void* p) { delete static_cast<std::vector<T>*>(p); });
  return view_of(dt, heap->data(), heap->size(), sizeof(T), owner);
}

// ---- errors ---------------------------------------------------------------------------------

py::object g_parse_error;

[[noreturn]] void raise_parse_error(const std::string& what, md::ParseStatus st, std::size_t line,
                                    std::string_view text) {
  py::object exc = g_parse_error(what);
  exc.attr("status") = std::string(md::to_string(st));
  exc.attr("line") = line;
  exc.attr("text") = std::string(text);
  PyErr_SetObject(g_parse_error.ptr(), exc.ptr());
  throw py::error_already_set();
}

[[noreturn]] void raise_file_error(const std::string& path, const char* why) {
  if (!std::filesystem::exists(path)) {
    PyErr_SetObject(PyExc_FileNotFoundError,
                    py::make_tuple(2, "No such file or directory", path).ptr());
  } else {
    PyErr_SetObject(PyExc_OSError, py::make_tuple(5, std::string(why), path).ptr());
  }
  throw py::error_already_set();
}

void open_or_raise(md::MappedFile& f, const std::string& path) {
  const char* err = nullptr;
  if (!f.open(path.c_str(), &err)) raise_file_error(path, err ? err : "cannot read");
}

py::object money_to_pyint(sim::Money2 v) {
  const std::string s = sim::format_fixed(v, 0);
  return py::reinterpret_steal<py::object>(PyLong_FromString(s.c_str(), nullptr, 10));
}

std::int64_t checked_i64(sim::Money2 v, const char* what) {
  if (v > static_cast<sim::Money2>(INT64_MAX) || v < static_cast<sim::Money2>(INT64_MIN)) {
    PyErr_Format(PyExc_OverflowError,
                 "%s does not fit in 64 bits; use SimResult.pnl for the exact integer", what);
    throw py::error_already_set();
  }
  return static_cast<std::int64_t>(v);
}

// ---- configuration classes with keyword constructors -----------------------------------------

#define BOOK_FIELDS(X) X(tick_size) X(base_price) X(num_levels) X(max_orders)
#define SIMCFG_FIELDS(X)                                                             \
  X(submit_latency_ns)                                                               \
  X(ack_latency_ns) X(md_latency_ns) X(maker_fee) X(taker_fee) X(sample_interval_ns) \
      X(record_fills)
#define AS_FIELDS(X)                                                                             \
  X(gamma)                                                                                       \
  X(k) X(sigma0) X(sigma_floor) X(horizon_s) X(finite_horizon) X(session_end_s) X(min_horizon_s) \
      X(order_size) X(max_inventory) X(size_skew) X(tick) X(min_price) X(max_price)              \
          X(requote_interval_ns) X(requote_threshold_ticks) X(sigma_interval_ns) X(sigma_alpha)

template <class T, class Setter>
T from_kwargs(const py::kwargs& kw, Setter set) {
  T v;
  for (auto item : kw) {
    const std::string k = py::cast<std::string>(item.first);
    if (!set(v, k, item.second)) throw py::type_error("unknown parameter '" + k + "'");
  }
  return v;
}

#define DEFINE_CONFIG(CLS, T, FIELDS, NAME)                                           \
  {                                                                                   \
    auto set = [](T& v, const std::string& k, py::handle val) {                       \
      FIELDS(SET_FIELD)                                                               \
      return false;                                                                   \
    };                                                                                \
    using Cfg = T;                                                                    \
    py::class_<T> c(m, NAME);                                                         \
    c.def(py::init([set](const py::kwargs& kw) { return from_kwargs<T>(kw, set); })); \
    FIELDS(DEF_RW)                                                                    \
    c.def("to_dict", [](const T& v) {                                                 \
      py::dict d;                                                                     \
      FIELDS(PUT_FIELD)                                                               \
      return d;                                                                       \
    });                                                                               \
    c.def("__repr__", [](const T& v) {                                                \
      py::dict d;                                                                     \
      FIELDS(PUT_FIELD)                                                               \
      return std::string(NAME) + "(" + std::string(py::str(d)) + ")";                 \
    });                                                                               \
    CLS = c;                                                                          \
  }

#define SET_FIELD(f)                                          \
  if (k == #f) {                                              \
    try {                                                     \
      v.f = py::cast<decltype(v.f)>(val);                     \
    } catch (const py::cast_error&) {                         \
      throw py::value_error("invalid value for '" + k + "'"); \
    }                                                         \
    return true;                                              \
  }
#define DEF_RW(f) c.def_readwrite(#f, &Cfg::f);
#define PUT_FIELD(f) d[#f] = v.f;

// ---- sink and engine wrappers ------------------------------------------------------------------

struct PySink {
  std::vector<Event> events;
  bool enabled = true;
  void operator()(const Event& e) {
    if (enabled) events.push_back(e);
  }
};

struct PyEngine {
  explicit PyEngine(const BookConfig& c) : eng(c) {}
  Engine<PySink> eng;
};

struct PyAdapter {
  explicit PyAdapter(PyEngine* e) : eng(e) {}
  PyEngine* eng;  // kept alive by keep_alive on the creating call
};

py::dict stats_dict(const BookStats& s) {
  py::dict d;
  py::dict rej;
  for (std::size_t r = 1; r < s.rejects.size(); ++r)
    rej[to_string(static_cast<RejectReason>(r))] = s.rejects[r];
  d["rejects"] = rej;
  d["trades"] = s.trades;
  d["feed_crossed_fills"] = s.feed_crossed_fills;
  d["feed_crossed_own_fills"] = s.feed_crossed_own_fills;
  d["feed_add_displaced_qty"] = s.feed_add_displaced_qty;
  d["feed_exec_overflow"] = s.feed_exec_overflow;
  d["queue_order_anomalies"] = s.queue_order_anomalies;
  d["anomalies"] = s.anomalies();
  d["feed_problems"] = s.feed_problems();
  return d;
}

// ---- simulation result
// ------------------------------------------------------------------------------

struct PySimResult {
  sim::RunResult r;
  std::vector<PathRow> path;
  py::dict strategy;
};

PySimResult finish_result(sim::RunResult&& r, const sim::AvellanedaStoikov& strat) {
  PySimResult out;
  out.r = std::move(r);
  out.path.reserve(out.r.path.size());
  for (const sim::PathSample& s : out.r.path) {
    PathRow row{};
    row.ts = s.ts;
    row.inventory = s.inventory;
    row.not_reported = s.known_inventory_lag;
    row.spread2 = checked_i64(s.spread2, "spread capture");
    row.inventory2 = checked_i64(s.inventory2, "inventory PnL");
    row.fees2 = checked_i64(s.fees2, "fees");
    row.total2 = checked_i64(s.spread2 + s.inventory2 - s.fees2, "total PnL");
    row.mid2 = s.mid2;
    out.path.push_back(row);
  }
  out.strategy["quotes_sent"] = strat.quotes_sent();
  out.strategy["cancels_sent"] = strat.cancels_sent();
  out.strategy["rejects"] = strat.rejects();
  out.strategy["final_sigma"] = strat.sigma();
  out.strategy["known_inventory"] = strat.known_inventory();
  return out;
}

sim::RunResult run_sim_on(const BookConfig& book, const sim::SimConfig& sc,
                          sim::AvellanedaStoikov& strat, sim::synthetic::SpanFeed& feed) {
  sim::Simulator<sim::AvellanedaStoikov, sim::synthetic::SpanFeed> sim(book, sc, strat, feed);
  return sim.run();
}

}  // namespace

PYBIND11_MODULE(_core, m) {
  m.doc() =
      "lobkit core bindings: limit order book engine, replay, LOBSTER reader, simulator. "
      "All bundled data generators produce SYNTHETIC data.";
  m.attr("PRICE_UNITS_PER_DOLLAR") = sim::kUnitsPerDollar;

  g_parse_error = py::reinterpret_steal<py::object>(
      PyErr_NewException("lobkit._core.ParseError", PyExc_ValueError, nullptr));
  m.attr("ParseError") = g_parse_error;
  // ParseError carries .status (parser status name), .line (1 based) and .text (the line).

  // ---- enums ----------------------------------------------------------------------------------
  py::enum_<Side>(m, "Side").value("Bid", Side::Bid).value("Ask", Side::Ask);
  py::enum_<OrderType>(m, "OrderType")
      .value("Limit", OrderType::Limit)
      .value("Market", OrderType::Market)
      .value("Ioc", OrderType::Ioc)
      .value("Fok", OrderType::Fok)
      .value("PostOnly", OrderType::PostOnly);
  py::enum_<EventType>(m, "EventType")
      .value("Ack", EventType::Ack)
      .value("Reject", EventType::Reject)
      .value("Trade", EventType::Trade)
      .value("Cancel", EventType::Cancel)
      .value("Modify", EventType::Modify);
  {
    py::enum_<RejectReason> e(m, "RejectReason");
    for (std::uint8_t i = 1; i < static_cast<std::uint8_t>(RejectReason::kCount); ++i)
      e.value(to_string(static_cast<RejectReason>(i)), static_cast<RejectReason>(i));
  }
  py::enum_<CancelReason>(m, "CancelReason")
      .value("UserRequest", CancelReason::UserRequest)
      .value("IocRemainder", CancelReason::IocRemainder)
      .value("MarketRemainder", CancelReason::MarketRemainder)
      .value("FeedReduce", CancelReason::FeedReduce)
      .value("FeedDelete", CancelReason::FeedDelete);

  // ---- dtypes exposed so callers can build arrays
  // -------------------------------------------------
  m.attr("EVENT_DTYPE") = event_dtype();
  m.attr("FEED_DTYPE") = feed_dtype();
  m.attr("FILL_DTYPE") = fill_dtype();
  m.attr("PATH_DTYPE") = path_dtype();

  // ---- configuration
  // ------------------------------------------------------------------------------------
  py::object book_cls, simcfg_cls, as_cls;
  DEFINE_CONFIG(book_cls, BookConfig, BOOK_FIELDS, "BookConfig")
  DEFINE_CONFIG(simcfg_cls, sim::SimConfig, SIMCFG_FIELDS, "SimConfig")
  DEFINE_CONFIG(as_cls, sim::ASParams, AS_FIELDS, "ASParams")
  book_cls.attr("validate") =
      py::cpp_function([](const BookConfig& c) { c.validate(); }, py::is_method(book_cls));
  // BookConfig.validate raises ValueError (std::invalid_argument) for a bad grid.

  // ---- Engine
  // ----------------------------------------------------------------------------------------------
  py::class_<PyEngine>(
      m, "Engine",
      "Single instrument price-time priority matching engine. Results are Events: "
      "drain_events() returns them as a numpy array (EVENT_DTYPE). Rejects are events, "
      "not exceptions.")
      .def(py::init([](const BookConfig& c) {
             c.validate();  // ValueError on a bad grid
             return std::make_unique<PyEngine>(c);
           }),
           py::arg("config"))
      .def(
          "submit",
          [](PyEngine& e, std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty,
             OrderType type, std::uint16_t owner,
             std::uint64_t ts) { e.eng.submit({id, price, qty, side, type, owner, ts}); },
          py::arg("id"), py::arg("side"), py::arg("price"), py::arg("qty"),
          py::arg("type") = OrderType::Limit, py::arg("owner") = 0, py::arg("ts") = 0)
      .def(
          "cancel", [](PyEngine& e, std::uint64_t id, std::uint64_t ts) { e.eng.cancel(id, ts); },
          py::arg("id"), py::arg("ts") = 0)
      .def(
          "modify",
          [](PyEngine& e, std::uint64_t id, std::int64_t price, std::uint32_t qty,
             std::uint64_t ts) { e.eng.modify({id, price, qty, ts}); },
          py::arg("id"), py::arg("new_price"), py::arg("new_qty"), py::arg("ts") = 0)
      .def(
          "replay_adapter", [](PyEngine& e) { return PyAdapter(&e); }, py::keep_alive<0, 1>())
      .def("drain_events",
           [](PyEngine& e) {
             // Moves the recorded events out. The returned array views the moved buffer (no copy),
             // is read-only, and owns it; the engine continues with an empty list.
             auto ev = std::move(e.eng.sink().events);
             e.eng.sink().events.clear();
             return adopt(std::move(ev), event_dtype());
           })
      .def_property(
          "record_events", [](PyEngine& e) { return e.eng.sink().enabled; },
          [](PyEngine& e, bool on) { e.eng.sink().enabled = on; })
      .def("best_bid",
           [](PyEngine& e) -> std::optional<std::int64_t> {
             if (!e.eng.book().has_bid()) return std::nullopt;
             return e.eng.book().best_bid();
           })
      .def("best_ask",
           [](PyEngine& e) -> std::optional<std::int64_t> {
             if (!e.eng.book().has_ask()) return std::nullopt;
             return e.eng.book().best_ask();
           })
      .def("depth_at", [](PyEngine& e, std::int64_t price) { return e.eng.book().depth_at(price); })
      .def("queue_ids",
           [](PyEngine& e, std::int64_t price) { return e.eng.book().queue_ids(price); })
      .def(
          "top_levels",
          [](PyEngine& e, Side side, std::size_t n) {
            // (k, 2) int64 array of (price, quantity), best first. A copy: it is small.
            std::vector<Book<PySink>::LevelView> lv(n);
            const std::size_t k = e.eng.book().top_levels(side, lv.data(), n);
            py::array_t<std::int64_t> out(
                {static_cast<py::ssize_t>(k), static_cast<py::ssize_t>(2)});
            auto r = out.mutable_unchecked<2>();
            for (std::size_t i = 0; i < k; ++i) {
              r(static_cast<py::ssize_t>(i), 0) = lv[i].price;
              r(static_cast<py::ssize_t>(i), 1) = static_cast<std::int64_t>(lv[i].qty);
            }
            return out;
          },
          py::arg("side"), py::arg("n") = 5)
      .def_property_readonly("live_orders", [](PyEngine& e) { return e.eng.book().live_orders(); })
      .def_property_readonly("last_seq", [](PyEngine& e) { return e.eng.book().last_seq(); })
      .def("stats", [](PyEngine& e) { return stats_dict(e.eng.book().stats()); })
      .def("check_invariants", [](PyEngine& e) -> std::optional<std::string> {
        return check_invariants(e.eng.book());
      });

  py::class_<PyAdapter>(m, "ReplayAdapter",
                        "Applies exogenous feed messages (owner 0) to the engine it came from; see "
                        "MATCHING_RULES.md sections 7 and 9.")
      .def(
          "apply_add",
          [](PyAdapter& a, std::uint64_t id, Side side, std::int64_t price, std::uint32_t qty,
             std::uint64_t ts) { a.eng->eng.replay_adapter().apply_add(id, side, price, qty, ts); },
          py::arg("id"), py::arg("side"), py::arg("price"), py::arg("qty"), py::arg("ts") = 0)
      .def(
          "apply_reduce",
          [](PyAdapter& a, std::uint64_t id, std::uint32_t qty, std::uint64_t ts) {
            a.eng->eng.replay_adapter().apply_reduce(id, qty, ts);
          },
          py::arg("id"), py::arg("qty"), py::arg("ts") = 0)
      .def(
          "apply_delete",
          [](PyAdapter& a, std::uint64_t id, std::uint64_t ts) {
            a.eng->eng.replay_adapter().apply_delete(id, ts);
          },
          py::arg("id"), py::arg("ts") = 0)
      .def(
          "apply_execute",
          [](PyAdapter& a, std::uint64_t id, std::uint32_t qty, std::uint64_t ts) {
            a.eng->eng.replay_adapter().apply_execute(id, qty, ts);
          },
          py::arg("id"), py::arg("qty"), py::arg("ts") = 0);

  // ---- LOBSTER
  // ------------------------------------------------------------------------------------------------
  m.def(
      "read_lobster_messages",
      [](const std::string& path, std::uint64_t limit) {
        md::MappedFile f;
        open_or_raise(f, path);
        md::lobster::MessageReader rd(f.text());
        std::vector<md::FeedMsg> out;
        md::FeedMsg msg;
        md::ParseStatus st;
        while ((limit == 0 || out.size() < limit) && (st = rd.next(msg)) == md::ParseStatus::Ok)
          out.push_back(msg);
        if (st != md::ParseStatus::End && st != md::ParseStatus::Ok)
          raise_parse_error(
              path + ":" + std::to_string(rd.line_number()) + ": " + md::to_string(st), st,
              rd.line_number(), rd.last_line());
        // The parsed messages are moved into an array that owns them (no second copy).
        return adopt(std::move(out), feed_dtype());
      },
      py::arg("path"), py::arg("limit") = 0,
      "Parses a LOBSTER message file into a read-only FEED_DTYPE array. Raises ParseError on "
      "malformed "
      "input, FileNotFoundError / OSError on I/O problems.");

  m.def(
      "read_lobster_book",
      [](const std::string& path, std::uint32_t levels) {
        md::MappedFile f;
        open_or_raise(f, path);
        if (levels == 0) levels = md::lobster::detect_levels(f.text());
        md::lobster::BookRowReader rd(f.text(), levels);
        if (rd.status() != md::ParseStatus::Ok)
          raise_parse_error(path + ": cannot determine the number of levels", rd.status(), 0, "");
        std::vector<std::int64_t> flat;
        md::lobster::BookRow row;
        md::ParseStatus st;
        while ((st = rd.next(row)) == md::ParseStatus::Ok)
          for (std::uint32_t i = 0; i < levels; ++i) {
            flat.push_back(row.ask[i].price);
            flat.push_back(row.ask[i].size);
            flat.push_back(row.bid[i].price);
            flat.push_back(row.bid[i].size);
          }
        if (st != md::ParseStatus::End)
          raise_parse_error(
              path + ":" + std::to_string(rd.line_number()) + ": " + md::to_string(st), st,
              rd.line_number(), rd.last_line());
        const std::size_t rows = flat.size() / (4u * levels);
        auto* heap = new std::vector<std::int64_t>(std::move(flat));
        py::capsule owner(heap, [](void* p) { delete static_cast<std::vector<std::int64_t>*>(p); });
        py::array a(py::dtype::of<std::int64_t>(),
                    {static_cast<py::ssize_t>(rows), static_cast<py::ssize_t>(levels),
                     static_cast<py::ssize_t>(4)},
                    {static_cast<py::ssize_t>(levels) * 32, py::ssize_t{32}, py::ssize_t{8}},
                    heap->data(), owner);
        make_readonly(a);
        return a;
      },
      py::arg("path"), py::arg("levels") = 0,
      "Parses a LOBSTER order book file into a read-only int64 array of shape (rows, levels, 4) "
      "with columns "
      "ask price, ask size, bid price, bid size. Missing levels keep LOBSTER dummy values.");

  m.def(
      "replay_lobster",
      [](PyEngine& e, const std::string& path, std::uint64_t limit) {
        md::MappedFile f;
        open_or_raise(f, path);
        md::lobster::MessageReader rd(f.text());
        md::ReplayCounters c;
        const bool was = e.eng.sink().enabled;
        e.eng.sink().enabled = false;  // millions of events would otherwise pile up in memory
        md::RunResult rr;
        {
          py::gil_scoped_release nogil;
          rr = md::replay(e.eng, rd, c, nullptr, limit);
        }
        e.eng.sink().enabled = was;
        if (rr.status != md::ParseStatus::End)
          raise_parse_error(
              path + ":" + std::to_string(rd.line_number()) + ": " + md::to_string(rr.status),
              rr.status, rd.line_number(), rd.last_line());
        py::dict d;
        d["messages"] = c.messages;
        d["adds"] = c.adds;
        d["reduces"] = c.reduces;
        d["deletes"] = c.deletes;
        d["executes"] = c.executes;
        d["hidden_executions"] = c.hidden_executions;
        d["hidden_shares"] = c.hidden_shares;
        d["halts"] = c.halts;
        d["cross_trades"] = c.cross_trades;
        d["seconds"] = rr.seconds;
        return d;
      },
      py::arg("engine"), py::arg("path"), py::arg("limit") = 0,
      "Replays a LOBSTER message file through the engine (event recording is suspended during the "
      "call). "
      "Returns counters; book anomalies are in engine.stats().");

  m.def(
      "price_range",
      [](py::array msgs) -> std::optional<std::pair<std::int64_t, std::int64_t>> {
        if (msgs.size() == 0) return std::nullopt;
        // (arrays are checked in run_simulation; this helper accepts the same dtype)
        auto flat = py::array::ensure(msgs);
        const auto* p = static_cast<const md::FeedMsg*>(flat.data());
        bool any = false;
        std::int64_t lo = 0, hi = 0;
        for (py::ssize_t i = 0; i < flat.size(); ++i) {
          if ((p[i].type != md::FeedType::Add && p[i].type != md::FeedType::Replace) ||
              p[i].price <= 0)
            continue;
          lo = any ? std::min(lo, p[i].price) : p[i].price;
          hi = any ? std::max(hi, p[i].price) : p[i].price;
          any = true;
        }
        if (!any) return std::nullopt;
        return std::make_pair(lo, hi);
      },
      py::arg("messages"),
      "(lowest, highest) displayed order price in a FEED_DTYPE array, or None.");

  m.def("suggest_base_price", &md::suggest_base_price, py::arg("lo"), py::arg("hi"),
        py::arg("tick"), py::arg("num_levels"));

  // ---- synthetic data
  // -----------------------------------------------------------------------------------------------
  m.def(
      "synthetic_feed",
      [](std::uint64_t seed, std::size_t messages, std::int64_t mid0, std::int64_t tick,
         double exec_share) {
        sim::synthetic::SynthParams sp;
        sp.seed = seed;
        sp.messages = messages;
        sp.mid0 = mid0;
        sp.tick = tick;
        sp.exec_share = exec_share;
        return adopt(sim::synthetic::make_synth_feed(sp), feed_dtype());
      },
      py::arg("seed") = 1, py::arg("messages") = 20000, py::arg("mid0") = 1000000,
      py::arg("tick") = 100, py::arg("exec_share") = 0.30,
      "SYNTHETIC random order book feed (FEED_DTYPE array; not market data). Deterministic in its "
      "arguments.");

  // ---- simulation
  // --------------------------------------------------------------------------------------------------------
  py::class_<PySimResult>(
      m, "SimResult",
      "Result of a simulation. fills and path are read-only numpy arrays that VIEW memory owned by "
      "this object (they keep it alive). path is converted once from 128 bit integers.")
      .def_property_readonly("fills",
                             [](py::object self) {
                               auto& r = self.cast<PySimResult&>();
                               return view_of(fill_dtype(), r.r.fills.data(), r.r.fills.size(),
                                              sizeof(sim::FillRecord), self);
                             })
      .def_property_readonly("path",
                             [](py::object self) {
                               auto& r = self.cast<PySimResult&>();
                               return view_of(path_dtype(), r.path.data(), r.path.size(),
                                              sizeof(PathRow), self);
                             })
      .def_property_readonly("stats",
                             [](const PySimResult& r) {
                               const auto& s = r.r.stats;
                               py::dict d;
                               d["feed_messages"] = s.feed_messages;
                               d["feed_id_collisions"] = s.feed_id_collisions;
                               d["time_regressions"] = s.time_regressions;
                               d["orders_sent"] = s.orders_sent;
                               d["orders_accepted"] = s.orders_accepted;
                               d["orders_rejected"] = s.orders_rejected;
                               d["cancels_sent"] = s.cancels_sent;
                               d["cancels_confirmed"] = s.cancels_confirmed;
                               d["cancels_too_late"] = s.cancels_too_late;
                               d["fills"] = s.fills;
                               d["shares_filled"] = s.shares_filled;
                               d["maker_fills"] = s.maker_fills;
                               d["taker_fills"] = s.taker_fills;
                               d["self_trades"] = s.self_trades;
                               d["max_abs_inventory"] = s.max_abs_inventory;
                               d["market_updates"] = s.market_updates;
                               return d;
                             })
      .def_property_readonly(
          "pnl",
          [](const PySimResult& r) {
            const auto& p = r.r.pnl;
            py::dict d;
            // Exact integers in half native units (1 unit = 0.5e-4 dollars), plus
            // dollar floats for display only.
            d["spread2"] = money_to_pyint(p.spread2);
            d["inventory2"] = money_to_pyint(p.inventory2);
            d["fees2"] = money_to_pyint(p.fees2);
            d["total2"] = money_to_pyint(p.total2());
            d["total2_from_ledger"] = money_to_pyint(p.total2_from_ledger());
            d["inventory"] = p.inventory;
            d["mid2_final"] = p.mid2_final;
            const double to_usd = 1.0 / (2.0 * sim::kUnitsPerDollar);
            d["spread_capture_dollars"] = static_cast<double>(p.spread2) * to_usd;
            d["inventory_pnl_dollars"] = static_cast<double>(p.inventory2) * to_usd;
            d["fees_paid_dollars"] = static_cast<double>(p.fees2) * to_usd;
            d["total_dollars"] = static_cast<double>(p.total2()) * to_usd;
            d["identity_holds"] = p.total2() == p.total2_from_ledger();
            return d;
          })
      .def_property_readonly("book_stats",
                             [](const PySimResult& r) { return stats_dict(r.r.book); })
      .def_property_readonly("strategy", [](const PySimResult& r) { return r.strategy; })
      .def_property_readonly(
          "feed_status",
          [](const PySimResult& r) { return std::string(md::to_string(r.r.feed_status)); })
      .def_property_readonly("first_ts", [](const PySimResult& r) { return r.r.first_ts; })
      .def_property_readonly("last_ts", [](const PySimResult& r) { return r.r.last_ts; })
      .def("fingerprint", [](const PySimResult& r) { return sim::fingerprint(r.r); })
      .def(
          "write_csv",
          [](const PySimResult& r, const std::string& prefix) {
            const char* err = nullptr;
            if (!sim::write_csv(r.r, prefix, &err)) {
              PyErr_SetObject(
                  PyExc_OSError,
                  py::make_tuple(5, std::string(err ? err : "write error"), prefix).ptr());
              throw py::error_already_set();
            }
          },
          py::arg("prefix"),
          "Writes PREFIX_fills.csv, PREFIX_inventory.csv and PREFIX_pnl.csv, byte for byte what "
          "lobkit_sim_run writes.")
      .def(
          "summary",
          [](const PySimResult& r, const std::string& label) {
            return sim::summarize(r.r, label.c_str());
          },
          py::arg("data_label"));

  m.def(
      "run_simulation",
      [](const BookConfig& book, const sim::SimConfig& sc, const sim::ASParams& ap,
         py::array messages) {
        book.validate();
        if (!py::cast<bool>(messages.dtype().attr("__eq__")(feed_dtype())))
          throw py::value_error("messages must have dtype lobkit.FEED_DTYPE");
        if (messages.ndim() != 1 || !(messages.flags() & py::array::c_style))
          throw py::value_error("messages must be a one dimensional C contiguous array");
        sim::AvellanedaStoikov strat(ap);
        sim::synthetic::SpanFeed feed(static_cast<const md::FeedMsg*>(messages.data()),
                                      static_cast<std::size_t>(messages.size()));
        sim::RunResult r;
        {
          // The array is kept alive by the argument; no Python object is touched while the GIL is
          // released.
          py::gil_scoped_release nogil;
          r = run_sim_on(book, sc, strat, feed);
        }
        return finish_result(std::move(r), strat);
      },
      py::arg("book"), py::arg("sim"), py::arg("strategy"), py::arg("messages"),
      "Runs the Avellaneda-Stoikov simulation over a FEED_DTYPE array (read, not copied).");

  m.def(
      "run_simulation_file",
      [](const BookConfig& book, const sim::SimConfig& sc, const sim::ASParams& ap,
         const std::string& path) {
        book.validate();
        md::MappedFile f;
        open_or_raise(f, path);
        md::lobster::MessageReader reader(f.text());
        sim::AvellanedaStoikov strat(ap);
        sim::RunResult r;
        {
          py::gil_scoped_release nogil;
          sim::Simulator<sim::AvellanedaStoikov, md::lobster::MessageReader> sim(book, sc, strat,
                                                                                 reader);
          r = sim.run();
        }
        if (r.feed_status != md::ParseStatus::End)
          raise_parse_error(path + ":" + std::to_string(reader.line_number()) + ": " +
                                md::to_string(r.feed_status),
                            r.feed_status, reader.line_number(), reader.last_line());
        return finish_result(std::move(r), strat);
      },
      py::arg("book"), py::arg("sim"), py::arg("strategy"), py::arg("path"),
      "Runs the simulation straight from a LOBSTER message file, as lobkit_sim_run does.");
}
