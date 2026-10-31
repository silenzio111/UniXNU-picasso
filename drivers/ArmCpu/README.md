# ArmCpu for sweet

Based on UniXNU/ArmCpu `1951cc8444b340d7d8879e9418220cf29eb711d7`.
The local change reads `/arm-io/intc/virtual-timer-intid` before mapping the
GIC, validates a little-endian u32 in 16..31, and uses that PPI for both
redistributor enables and IRQ dispatch. Sweet supplies 19; Picasso's generated
ADT omits the property, so the board default is also 19.

SMP remains opt-in through upstream's `sheng-smp` argument. The first sweet
configuration describes one CPU. No new-kernel SMP result is claimed.
