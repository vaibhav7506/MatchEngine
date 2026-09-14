#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "matchengine/engine.hpp"

namespace py = pybind11;
using namespace matchengine;

PYBIND11_MODULE(matchengine_py, m) {
    m.doc() = "Value-oriented bindings to the unchanged Phase 1 matching engine";
    py::enum_<Side>(m, "Side").value("BUY", Side::BUY).value("SELL", Side::SELL);
    py::enum_<OrderType>(m, "OrderType")
        .value("LIMIT", OrderType::LIMIT).value("MARKET", OrderType::MARKET)
        .value("IOC", OrderType::IOC).value("FOK", OrderType::FOK);
    py::enum_<OrderStatus>(m, "OrderStatus")
        .value("NEW", OrderStatus::NEW).value("PARTIALLY_FILLED", OrderStatus::PARTIALLY_FILLED)
        .value("FILLED", OrderStatus::FILLED).value("CANCELLED", OrderStatus::CANCELLED)
        .value("REJECTED", OrderStatus::REJECTED);
    py::class_<Order>(m, "Order")
        .def(py::init([](OrderID id, TraderID trader, Side side, double price,
                         Quantity quantity, OrderType type, Timestamp timestamp,
                         Quantity remaining, OrderStatus status) {
            return Order{id, trader, side, price, quantity, remaining, timestamp, status, type};
        }), py::arg("order_id") = 0, py::arg("trader_id") = 0, py::arg("side") = Side::BUY,
            py::arg("price") = 0.0, py::arg("quantity") = 0, py::arg("type") = OrderType::LIMIT,
            py::arg("timestamp") = 0, py::arg("remaining_quantity") = 0, py::arg("status") = OrderStatus::NEW)
        .def_readwrite("order_id", &Order::order_id)
        .def_readwrite("trader_id", &Order::trader_id)
        .def_readwrite("side", &Order::side)
        .def_readwrite("price", &Order::price)
        .def_readwrite("quantity", &Order::quantity)
        .def_readwrite("remaining_quantity", &Order::remaining_quantity)
        .def_readwrite("timestamp", &Order::timestamp)
        .def_readwrite("status", &Order::status)
        .def_readwrite("type", &Order::type);
    py::class_<Trade>(m, "Trade")
        .def_readonly("trade_id", &Trade::trade_id)
        .def_readonly("price", &Trade::price)
        .def_readonly("quantity", &Trade::quantity)
        .def_readonly("timestamp", &Trade::timestamp)
        .def_readonly("aggressor_order_id", &Trade::aggressor_order_id)
        .def_readonly("resting_order_id", &Trade::resting_order_id)
        .def_readonly("aggressor_side", &Trade::aggressor_side);
    py::class_<TopOfBook>(m, "TopOfBook")
        .def_readonly("best_bid", &TopOfBook::best_bid).def_readonly("best_ask", &TopOfBook::best_ask)
        .def_readonly("bid_qty", &TopOfBook::bid_qty).def_readonly("ask_qty", &TopOfBook::ask_qty);
    py::class_<LevelSnapshot>(m, "LevelSnapshot")
        .def_readonly("price", &LevelSnapshot::price).def_readonly("quantity", &LevelSnapshot::quantity)
        .def_readonly("order_count", &LevelSnapshot::order_count);
    py::class_<BookSnapshot>(m, "BookSnapshot")
        .def_readonly("bids", &BookSnapshot::bids).def_readonly("asks", &BookSnapshot::asks);
    py::class_<Engine>(m, "Engine")
        .def(py::init<>())
        .def("add_order", &Engine::add_order, py::arg("order"))
        .def("cancel_order", &Engine::cancel_order, py::arg("order_id"))
        .def("modify_order", &Engine::modify_order, py::arg("order_id"), py::arg("new_price"), py::arg("new_qty"))
        .def("get_order_status", &Engine::get_order_status, py::arg("order_id"))
        .def("get_top_of_book", &Engine::get_top_of_book)
        .def("get_book_snapshot", &Engine::get_book_snapshot, py::arg("depth"))
        .def("resting_order_count", &Engine::resting_order_count);
}
