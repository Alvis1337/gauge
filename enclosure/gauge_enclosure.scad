// AutoGauge enclosure -- v2
// Two-piece clamshell: front shell (display window + standoffs + status
// LED window + LED/USB cable cutouts + power button) and a back cover
// (flat lid). Measured with calipers against the physical ST7796 4"
// display module, ESP32 DevKitC, buck converter, status LED, and power
// button -- see the "measured" block below. Anything in the "ASSUMED /
// PLACEHOLDER" block is a guess flagged for review, not a real measurement.
//
// v2 fixes (from the first print's test-fit):
//   1. Standoff posts used to start at the PCB's own layer with a 6mm OD
//      against the PCB's 3mm hole -- physically blocked the display from
//      seating. Posts now start AFTER the PCB's back face.
//   2. The LED tab overlapped the nearest corner post (~2.5mm center-to-
//      center vs >10mm needed) -- this was the stray "bump" on one hole.
//      Relocated the LED window to the right edge, biased toward the top,
//      clear of every post and the display window's margins (see note at
//      led_center below for why it's not literally in the corner).
//   3. "Clearance behind the PCB" is now one shared variable
//      (pcb_back_clearance) instead of two hardcoded numbers.
//   4. Status LED is 3.6mm thick with its wires (was modeled at 3.0mm bare)
//      -- diffuser skin recalculated down to ~0.8mm to compensate.
//   5. Removed the 3 placeholder 5mm wire holes -- too small for anything
//      real (USB cord + LED wire ended up sharing the button hole instead).
//   6/7. Added a proper 14x7mm LED cable slot and a 12x6mm USB cord cutout,
//      both through the left (X=0) side wall.
//   8. Removed the placeholder 30x30mm back-mount pad -- not needed until
//      a real mount/adapter is chosen.
//   9. Reworked screw-hole sizing: the lid's holes are a 3mm pass-through
//      (screws thread into the box, not the lid); the box's holes are a
//      2.85mm self-tapping pilot.
//  10. Removed the standoff post entirely (fix #1 turned out incomplete --
//      see below). ANY raised post in the compartment, wherever it starts
//      along Z, sits in the PCB's slide-in path from the open back to the
//      front wall, and since it's wider than the PCB's own hole, it always
//      blocks insertion -- moving its start height doesn't fix that, only
//      hides it at the resting position. Now the front wall's own hole is
//      the only grip point (flush, not raised, so nothing to slide past);
//      the screw spans the compartment as bare shank and the perimeter
//      wall's rim is what the back cover rests against.
//
// Coordinate convention: shell-local X+ = right, Y+ = up (matches how the
// board would sit face-up on the print bed for the front shell). Z+ = out
// of the front (viewer-facing) face; the front shell's outer face is at
// Z=0, its inner face at Z=front_wall_t.
//
// PRINT: front shell face-down (window on the bed) needs no supports for
// the window opening; the LED pocket's roof will need supports or a slower
// bridge setting since it's a horizontal cavity. Back cover prints flat,
// no supports needed.

// ---------------- measured: display PCB ----------------
disp_pcb_w = 108.0; // left-right -- corrected after 2nd print (was 107.5)
disp_pcb_h = 61.6;  // top-bottom -- corrected after 2nd print (was 61.5)
disp_pcb_t = 5.8;   // thickness, NOT including header pins on the back

// The interior cavity used to equal disp_pcb_w/h exactly -- a zero-
// clearance fit even under perfect printing. Combined with this printer
// running ~0.2mm undersized per dimension (measured: modeled interior
// 107.5x61.5 printed as 107.3x61.3), the PCB (now measured 108x61.6, i.e.
// bigger than the interior in BOTH prints) couldn't fit at all. Both
// numbers below are added to the cavity, not baked into disp_pcb_w/h
// itself, so window margins and mount-hole positions stay anchored to the
// PCB's true size.
pcb_fit_clearance = 0.6; // real slop beyond the PCB's own size, both axes
print_shrink_comp = 0.2; // offsets this printer's observed undersizing

// ---------------- measured: glass / viewable window ----------------
glass_w     = 94.5;
glass_h     = 60.8;
glass_proud = 4.5;  // how far the glass rises above the PCB's front face

// The printed window opening was too tight a fit for the actual screen --
// add clearance to the shorter dimension (height, 60.8mm vs 94.5mm width).
// Applied only to the cut itself (below), not this measured constant, and
// split evenly so the opening stays centered.
window_extra_h = 1.0;

// derived margins (glass centered on the PCB; matches "sides have 6.8mm,
// top/bottom don't" -- using the measured W/H pair directly keeps the
// window+PCB geometry internally consistent rather than mixing in the
// separately-stated 6.8mm, which was ~0.3mm/side off from this pair)
margin_lr = (disp_pcb_w - glass_w) / 2; // ~6.5mm each side
margin_tb = (disp_pcb_h - glass_h) / 2; // ~0.35mm each side

// ---------------- measured: mounting holes (4, one per corner) --------
hole_dia      = 3.0;
hole_x_offset = 1.5; // from left/right edge, both rows
hole_top_y    = 1.5; // top row, from top edge
hole_bottom_y = 2.0; // bottom row, from bottom edge

// ---------------- measured: ESP32 DevKitC ----------------
esp32_l = 53.6;
esp32_w = 28.0;
esp32_t = 1.5; // PCB only, not header pins

// ---------------- measured: status LED ----------------
led_dia        = 9.5;
led_t          = 3.6;  // thickest point INCLUDING wires (was 3.0mm bare)
led_clearance  = 2.0;  // pocket diameter = led_dia + this. Was 0.4mm (a near
                        // zero-tolerance press fit for a disc with wires
                        // already attached, no room to align/slide it in
                        // from inside the compartment) -- opened up to
                        // give real room to maneuver it into place. Only
                        // affects the radial fit, not depth, so it doesn't
                        // eat into the diffuser skin thickness at all.
led_diffuser_t = 0.8;  // wall left over the disc as a diffuser -- thinner than
                        // v1 (1.4mm) because led_t grew; still printable

// ---------------- measured: buck converter ----------------
buck_l = 66.0;
buck_w = 39.0;
buck_h = 12.5; // tallest point, including the digit display on top

// ---------------- measured: power button ----------------
button_dia = 15.5; // widest point (panel bezel/nut) -- used as the cut
                    // diameter directly; if the actual through-hole should
                    // be smaller than the visible bezel, shrink this

// ---------------- measured: cable cutouts ----------------
led_slot_w = 14.0; // LED wire/connector slot, through the left side wall
led_slot_h = 7.0;
usb_slot_w = 12.0; // USB cord cutout, same wall
usb_slot_h = 6.0;

// ---------------- ASSUMED / PLACEHOLDER -- verify before printing -----
wall               = 2.5; // general shell wall thickness
front_wall_t       = glass_proud + 0.1; // glass sits ~flush with the front face;
                                        // also sets the LED pocket depth (see below)
interior_depth     = 35;  // total compartment depth, front inner face to back cover
lid_hole_dia       = 3.0; // lid: plain pass-through, NOT tapped -- screws bite the box, not the lid
box_hole_dia       = 2.85;// box: self-tapping pilot, through the front wall only (fix #10)
pcb_back_clearance = 2;   // gap behind the PCB's back face (header pins, solder) --
                          // used by the internal_reference fit-check
corner_r           = 3;   // shell outer corner rounding

// front_wall_t (4.6mm) minus led_diffuser_t (0.8mm) = 3.8mm pocket depth,
// which must be >= led_t (3.6mm) for the disc+wires to fit -- it is, with
// 0.2mm of clearance. If you change led_diffuser_t or led_t, re-check this.
led_pocket_depth = front_wall_t - led_diffuser_t;

cavity_extra = pcb_fit_clearance + print_shrink_comp; // total slack added to the interior, both axes
shell_w = disp_pcb_w + cavity_extra + 2*wall;
shell_h = disp_pcb_h + cavity_extra + 2*wall;

module rounded_rect(w, h, r) {
    hull() {
        for (x = [r, w-r]) for (y = [r, h-r])
            translate([x, y]) circle(r=r, $fn=48);
    }
}

// Cuts a cable slot through the X=0 side wall: a plain rectangle (the
// actual cable clearance) topped with a 45-degree triangular peak instead
// of a flat ceiling, so the cutout is fully self-supporting -- no bridging
// needed when printed with this wall vertical. w/h size the rectangle
// (the peak adds extra clearance above that, apex height = w/2).
module peaked_slot(y_center, z_center, w, h) {
    ap = w/2;
    translate([-1, y_center, 0])
        rotate([0, 90, 0])
            linear_extrude(wall + 2)
                polygon(points=[
                    [-(z_center - h/2), -w/2],
                    [-(z_center - h/2),  w/2],
                    [-(z_center + h/2),  w/2],
                    [-(z_center + h/2 + ap), 0],
                    [-(z_center + h/2), -w/2],
                ]);
}

// PCB's bottom-left corner sits at shell-local (wall, wall); mount holes
// and the window are positioned from there.
function mount_hole_positions() = [
    [wall + hole_x_offset,              wall + hole_top_y],                 // A: low X, low Y
    [wall + disp_pcb_w - hole_x_offset, wall + hole_top_y],                 // B: high X, low Y
    [wall + hole_x_offset,              wall + disp_pcb_h - hole_bottom_y], // C: low X, high Y
    [wall + disp_pcb_w - hole_x_offset, wall + disp_pcb_h - hole_bottom_y], // D: high X, high Y
];

// Status LED window position (shared XY between front_shell's posts and
// back_cover's tab, even though the tab itself now lives on back_cover --
// exposed from behind the enclosure, not the front). Originally wanted
// "front face, top corner", but every corner is already occupied by a
// mounting post (radius 3mm), the LED tab needs a large radius, and the
// display window leaves only ~0.35mm margin top/bottom and ~6.5mm margin
// left/right -- nowhere near enough room for the two features to coexist
// in an actual corner. This places it on the right edge instead (X =
// shell_w, mostly external), biased toward the top (Y=45 out of
// shell_h=66.5) and far from both right-side posts (B and D, >17mm away).
led_center = [shell_w, 45];

// Bottom edge (Y=0 side wall): power button only now -- the 3 placeholder
// wire holes are gone (fix #5); LED/USB cable cutouts moved to the left
// side wall instead (fix #6/#7), sized to what's actually going through
// them rather than generic small holes.
bottom_hole_z = front_wall_t + interior_depth/2;
button_x      = shell_w/2;

// Left side wall (X=0) cable cutouts -- Y positions chosen clear of both
// left-side posts (A and C, at Y=4 and Y=62) with plenty of margin.
led_slot_y  = 45;
led_slot_z  = front_wall_t + 15;
usb_slot_y  = 20;
usb_slot_z  = front_wall_t + 15;

module front_shell() {
    difference() {
        union() {
            linear_extrude(front_wall_t)
                rounded_rect(shell_w, shell_h, corner_r);
            // LED corner tab: extra material so the pocket has somewhere to
            // live outside the PCB envelope -- see led_center comment above
            // for why this isn't in the literal corner. Sized off the
            // pocket's own (clearance-inclusive) diameter, not the bare
            // disc, so the wall around it stays a consistent `wall` thick
            // regardless of how much radial clearance the pocket needs.
            translate([led_center[0], led_center[1], 0])
                linear_extrude(front_wall_t)
                    circle(d = led_dia + led_clearance + 2*wall, $fn=64);
            // perimeter side walls -- encloses the ESP32/wiring compartment
            // on all 4 sides; without this the posts are the only thing
            // between "inside" and "outside".
            translate([0, 0, front_wall_t])
                linear_extrude(interior_depth)
                    difference() {
                        rounded_rect(shell_w, shell_h, corner_r);
                        translate([wall, wall])
                            rounded_rect(shell_w - 2*wall, shell_h - 2*wall,
                                        max(corner_r - wall, 0.1));
                    }
        }
        // display window -- height grown by window_extra_h (see above),
        // centered on the same spot as before
        translate([wall + margin_lr, wall + margin_tb - window_extra_h/2, -1])
            linear_extrude(front_wall_t + 2)
                square([glass_w, glass_h + window_extra_h]);
        // status LED pocket -- cut from the inside (back) face, leaving
        // led_diffuser_t of skin facing the viewer
        translate([led_center[0], led_center[1], front_wall_t - led_pocket_depth])
            linear_extrude(led_pocket_depth + 1)
                circle(d = led_dia + led_clearance, $fn=64);

        // power button -- through the bottom (Y=0) side wall
        translate([button_x, -1, bottom_hole_z])
            rotate([-90, 0, 0])
                cylinder(d = button_dia, h = wall + 2, $fn = 48);

        // LED wire/connector slot + USB cord cutout -- through the left
        // (X=0) side wall, sized to the actual cables going through them.
        // Peaked top (not flat) so both are self-supporting when printed
        // with this wall vertical -- see peaked_slot() above.
        peaked_slot(led_slot_y, led_slot_z, led_slot_w, led_slot_h);
        peaked_slot(usb_slot_y, usb_slot_z, usb_slot_w, usb_slot_h);

        // box-side screw bore -- the ONLY self-tapping happens here, through
        // the front wall's own solid material (the main slab above is
        // already solid at these XY spots, this just drills it). No raised
        // standoff post anywhere in the compartment on purpose (fix #10):
        // a post here -- wherever along Z it's placed -- sits in the exact
        // path the PCB has to slide through from the open back to reach
        // the front wall, and since it's wider (6mm) than the PCB's own
        // hole (3mm), it blocks the PCB from ever sliding in. The screw
        // instead spans the full compartment as bare, unsupported shank
        // (fine mechanically -- it just needs the one grip point here at
        // the front, plus its head bearing on the back cover), and the
        // perimeter side walls' rim (not a corner post) is what the back
        // cover naturally rests against when closed. Use a long screw
        // (~front_wall_t + interior_depth + wall, so ~40mm here).
        for (p = mount_hole_positions())
            translate([p[0], p[1], -1])
                cylinder(d = box_hole_dia, h = front_wall_t + 2, $fn=24);
    }
}

module back_cover() {
    difference() {
        linear_extrude(wall)
            rounded_rect(shell_w, shell_h, corner_r);
        // Plain pass-through (fix #9) -- screws bite the box's self-tapping
        // pilot, not the lid, so this just needs to clear an M3 shank.
        for (p = mount_hole_positions())
            translate([p[0], p[1], -1])
                cylinder(d = lid_hole_dia, h = wall + 2, $fn=24);
    }
    // (fix #8: the placeholder back-mount pad is gone -- add a real one
    // once a specific mount/adapter is picked)
}

// Visual-only fit check -- NOT part of the actual print (the leading `%`
// renders these transparent in preview and excludes them from CSG/export).
// Positioned in the free floor area behind the display's back face,
// side by side; move these if your real board layout differs.
module internal_reference() {
    z0 = front_wall_t + disp_pcb_t + pcb_back_clearance;
    // buck converter
    %translate([wall + 3, wall + 3, z0])
        cube([buck_l, buck_w, buck_h]);
    // ESP32 (component height above bare PCB is a guess -- 8mm placeholder)
    %translate([wall + buck_l + 8, wall + 3, z0])
        cube([esp32_w, esp32_l, 8]);
}

// ---------------- preview layout ----------------
front_shell();
internal_reference();
translate([shell_w + 20, 0, 0]) back_cover();

echo(str("shell size: ", shell_w, " x ", shell_h, " mm"));
echo(str("LED pocket depth: ", led_pocket_depth, " mm (must be >= ", led_t, ")"));
echo(str("floor space check: buck+ESP32 width used = ", wall + buck_l + 8 + esp32_w,
        " mm (must be <= shell interior width ", shell_w - wall, ")"));
echo(str("required screw length (approx): ", front_wall_t + interior_depth + wall, " mm"));
