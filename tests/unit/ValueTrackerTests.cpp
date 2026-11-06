#include "Test.hpp"

TEST_CASE("Double-free when overwriting split driver intervals",
          "[ValueTracker]") {
  // When an existing [0:7] entry is split by a narrower assignment (t[3]=b),
  // both halves [0:2] and [4:7] reference the same DriverListHandle.  A
  // subsequent full-width replacement (t=c) iterated over both and freed that
  // shared handle twice, causing undefined behaviour.
  auto const &tree = R"(
module m (input logic [7:0] a, b, c, output logic [7:0] out);
  logic [7:0] t;
  always_comb begin
    t = a;        // creates [0:7] entry
    t[3] = b[0]; // splits [0:7]: [0:2] and [4:7] share the same handle
    t = c;        // replaces [0:7]: iterates both shared-handle entries
  end
  assign out = t;
endmodule
)";
  NetlistTest test(tree);
  // The final t=c overwrites all previous drivers; only c reaches out.
  CHECK(test.pathExists("m.c", "m.out"));
  CHECK_FALSE(test.pathExists("m.a", "m.out"));
}

TEST_CASE("Inverted bounds when new range contains existing with same "
          "upper edge (merge)",
          "[ValueTracker]") {
  // When merging if/else branches where one drives t[7:4] and the other t[7:0],
  // the "new bounds contains existing" merge path incremented bounds.left to 8
  // after consuming the [4:7] entry, which exceeded bounds.right (7) and caused
  // an IntervalMap insertion with an inverted key.
  auto const &tree = R"(
module m (input logic [7:0] a, b, input logic c, output logic [7:0] out);
  logic [7:0] t;
  always_comb begin
    if (c)
      t[7:4] = a[3:0];
    else
      t[7:0] = b;
  end
  assign out = t;
endmodule
)";
  NetlistTest test(tree);
  // Bits [7:4] can be driven from either branch; bits [3:0] only from else.
  CHECK(test.getDrivers("m.t", {7, 4}).size() == 2);
  CHECK(test.getDrivers("m.t", {3, 0}).size() == 1);
  CHECK(test.pathExists("m.a", "m.out"));
  CHECK(test.pathExists("m.b", "m.out"));
}

TEST_CASE("Range fully consumed after contains-left overlap (replace)",
          "[ValueTracker]") {
  // When replacing drivers, the "new contains existing" path adjusts
  // bounds.left past the existing entry. If the consumed entry aligns
  // exactly with the right edge, bounds.left > bounds.right and we
  // should return early.
  auto const &tree = R"(
module m(input logic [3:0] a, b, output logic [3:0] out);
  logic [3:0] t;
  always_comb begin
    t[3:2] = a[1:0];
    t[3:0] = b;
  end
  assign out = t;
endmodule
)";
  NetlistTest test(tree);
  // After t[3:0] = b, only b drives all of t.
  CHECK(test.getDrivers("m.t", {3, 0}).size() == 1);
  CHECK(test.pathExists("m.b", "m.out"));
  CHECK_FALSE(test.pathExists("m.a", "m.out"));
}

TEST_CASE("Range fully consumed after left-overlap (replace)",
          "[ValueTracker]") {
  // The left-overlap replace path adjusts bounds.left. If the consumed
  // entry's upper edge equals bounds.right, we hit the early return
  // early return.
  auto const &tree = R"(
module m(input logic [3:0] a, b, output logic [3:0] out);
  logic [3:0] t;
  always_comb begin
    t[3:0] = a;
    t[3:1] = b[2:0];
  end
  assign out = t;
endmodule
)";
  NetlistTest test(tree);
  // Bit 0 driven by a, bits [3:1] driven by b.
  CHECK(test.getDrivers("m.t", {0, 0}).size() == 1);
  CHECK(test.getDrivers("m.t", {3, 1}).size() == 1);
}

TEST_CASE("Inverted bounds when new range left-overlaps existing with "
          "same upper edge (merge)",
          "[ValueTracker]") {
  // When merging if/else branches where one drives t[7:0] and the other t[7:3],
  // the "left-overlap" path incremented bounds.left to 8 after consuming the
  // existing [0:7] entry, which exceeded bounds.right (7) and caused an
  // IntervalMap insertion with an inverted key.
  auto const &tree = R"(
module m (input logic [7:0] a, b, input logic c, output logic [7:0] out);
  logic [7:0] t;
  always_comb begin
    if (c)
      t[7:0] = a;
    else
      t[7:3] = b[4:0];
  end
  assign out = t;
endmodule
)";
  NetlistTest test(tree);
  // Bits [7:3] can be driven from either branch; bits [2:0] only from if.
  CHECK(test.getDrivers("m.t", {7, 3}).size() == 2);
  CHECK(test.getDrivers("m.t", {2, 0}).size() == 1);
  CHECK(test.pathExists("m.a", "m.out"));
  CHECK(test.pathExists("m.b", "m.out"));
}

TEST_CASE("Use-after-free when overwriting one half of a split interval "
          "(nonblocking)",
          "[ValueTracker]") {
  // t <= 0 creates [0:31]; t[16] <= a splits it into [0:15] and [17:31],
  // which shared one driver-list handle. t[31:16] <= d contains [16] and
  // [17:31], so erasing [17:31] freed the handle while [0:15] still mapped
  // to it, and the next use of [0:15] read a freed list (segfault).
  auto const &tree = R"(
module m (input logic clk, a, input logic [15:0] d, output logic [31:0] out);
  logic [31:0] t;
  always_ff @(posedge clk) begin
    t <= 0;
    t[16] <= a;
    t[31:16] <= d;
  end
  assign out = t;
endmodule
)";
  NetlistTest test(tree);
  CHECK(test.pathExists("m.d", "m.out"));
}

TEST_CASE("Writing one half of a split interval leaves the other half's "
          "drivers alone (replace)",
          "[ValueTracker]") {
  // The halves of a split interval must not share a driver list: replacing
  // the drivers of [17:31] cleared and refilled the shared list, so a read of
  // t[3:0] in [0:15] saw d's assignment instead of t = 0.
  auto const &tree = R"(
module m (input logic a, input logic [15:0] d, output logic [3:0] lo);
  logic [31:0] t;
  always_comb begin
    t = 0;
    t[16] = a;
    t[31:17] = d[14:0];
    lo = t[3:0];
  end
endmodule
)";
  NetlistTest test(tree);
  CHECK_FALSE(test.pathExists("m.d", "m.lo"));
  CHECK_FALSE(test.pathExists("m.a", "m.lo"));
}

TEST_CASE("Writing one half of a split interval leaves the other half's "
          "drivers alone (merge)",
          "[ValueTracker]") {
  // Same as above, but the write sits in a branch, so the join merges it
  // into the existing [17:31] list, which [0:15] shared.
  auto const &tree = R"(
module m (input logic a, c, input logic [15:0] d, output logic [3:0] lo,
          output logic [14:0] hi);
  logic [31:0] t;
  always_comb begin
    t = 0;
    t[16] = a;
    if (c)
      t[31:17] = d[14:0];
    lo = t[3:0];
    hi = t[31:17];
  end
endmodule
)";
  NetlistTest test(tree);
  CHECK_FALSE(test.pathExists("m.d", "m.lo"));
  CHECK(test.pathExists("m.d", "m.hi"));
}
