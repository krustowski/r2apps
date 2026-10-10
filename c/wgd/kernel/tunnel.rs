//! One userspace IPv4 tunnel. The ABI contains no crypto or WireGuard keys.
//! Caller holds interrupts disabled while accessing the registration.

const NONE: usize = 0xff;
pub const CAPABILITY: u64 = 0x52325748; // R2WG ABI 2
pub const ROUTES_MAX: usize = 16;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Route {
    pub network: [u8; 4],
    pub prefix: u8,
    pub reserved: [u8; 3],
}
const EMPTY_ROUTE: Route = Route {
    network: [0; 4],
    prefix: 0,
    reserved: [0; 3],
};

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Config {
    pub local: [u8; 4],
    pub port: u16,
    pub mtu: u16,
    pub count: u8,
    pub reserved: [u8; 3],
    pub routes: [Route; ROUTES_MAX],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Probe {
    pub target: [u8; 4],
    pub local: [u8; 4],
    pub id: u16,
    pub reserved: u16,
}

#[derive(Clone, Copy)]
struct State {
    owner: usize,
    config: Config,
    probe_owner: usize,
    probe: Probe,
}
impl State {
    const fn empty() -> Self {
        Self {
            owner: NONE,
            config: Config {
                local: [0; 4],
                port: 0,
                mtu: 0,
                count: 0,
                reserved: [0; 3],
                routes: [EMPTY_ROUTE; ROUTES_MAX],
            },
            probe_owner: NONE,
            probe: Probe {
                target: [0; 4],
                local: [0; 4],
                id: 0,
                reserved: 0,
            },
        }
    }
    fn attach(&mut self, pid: usize, config: Config) -> bool {
        fn unicast(ip: [u8; 4]) -> bool {
            ip[0] != 0 && ip[0] != 127 && ip[0] < 224
        }
        if pid == NONE
            || !unicast(config.local)
            || config.port == 0
            || !(576..=1420).contains(&config.mtu)
            || config.count == 0
            || config.count as usize > ROUTES_MAX
            || config.reserved != [0; 3]
            || (self.owner != NONE && self.owner != pid)
        {
            return false;
        }
        for r in &config.routes[..config.count as usize] {
            if r.prefix > 32
                || r.reserved != [0; 3]
                || u32::from_be_bytes(r.network) & !mask(r.prefix) != 0
            {
                return false;
            }
        }
        *self = Self {
            owner: pid,
            config,
            ..Self::empty()
        };
        true
    }
    fn detach(&mut self, pid: usize) -> bool {
        if self.probe_owner == pid {
            self.probe_owner = NONE;
        }
        if self.owner != NONE && self.owner != pid {
            return false;
        }
        *self = Self::empty();
        true
    }
    fn udp_owner(&self, frame: &[u8]) -> Option<usize> {
        let ip = ipv4(frame)?;
        let ihl = (ip[0] & 15) as usize * 4;
        if self.owner != NONE
            && ip[9] == 17
            && ip.len() >= ihl + 8
            && ip[16..20] != self.config.local
            && ip[12..16] != self.config.local
            && u16::from_be_bytes([ip[ihl + 2], ip[ihl + 3]]) == self.config.port
        {
            Some(self.owner)
        } else {
            None
        }
    }
    fn outbound(&self, frame: &[u8], sender: usize) -> bool {
        // The daemon's outer UDP always uses the physical NIC, never recurses.
        self.owner != NONE
            && sender != self.owner
            && frame.len() >= 34
            && frame[12..14] == [8, 0]
            && frame[26..30] == self.config.local
            && self.allowed([frame[30], frame[31], frame[32], frame[33]])
    }
    fn reject_nic(&self, frame: &[u8]) -> bool {
        self.owner != NONE
            && frame.len() >= 34
            && frame[12..14] == [8, 0]
            && (frame[26..30] == self.config.local || frame[30..34] == self.config.local)
    }
    fn accepts(&self, pid: usize, frame: &[u8]) -> bool {
        self.owner == pid
            && self.owner != NONE
            && ipv4(frame).is_some_and(|ip| {
                ip.len() <= self.config.mtu as usize
                    && self.allowed([ip[12], ip[13], ip[14], ip[15]])
                    && ip[12..16] != self.config.local
                    && ip[16..20] == self.config.local
                    && (ip[9] == 6 || ip[9] == 1)
            })
    }
    fn allowed(&self, ip: [u8; 4]) -> bool {
        self.owner != NONE
            && self.config.routes[..self.config.count as usize]
                .iter()
                .any(|r| u32::from_be_bytes(ip) & mask(r.prefix) == u32::from_be_bytes(r.network))
    }
    fn claim_probe(&mut self, pid: usize, target: [u8; 4], id: u16) -> Option<Probe> {
        if pid == NONE
            || pid == self.owner
            || !self.allowed(target)
            || target == self.config.local
            || target[0] == 0
            || target[0] == 127
            || target[0] >= 224
            || (self.probe_owner != NONE && self.probe_owner != pid)
        {
            return None;
        }
        self.probe_owner = pid;
        self.probe = Probe {
            target,
            local: self.config.local,
            id,
            reserved: 0,
        };
        Some(self.probe)
    }
    fn icmp_owner(&self, frame: &[u8]) -> Option<usize> {
        if self.probe_owner == NONE {
            return None;
        }
        let ip = ipv4(frame)?;
        let ihl = (ip[0] & 15) as usize * 4;
        if ip[9] != 1 || ip[16..20] != self.config.local || ip.len() < ihl + 8 {
            return None;
        }
        let icmp = &ip[ihl..];
        let original = match icmp[0] {
            0 if icmp[1] == 0 && ip[12..16] == self.probe.target => icmp,
            3 | 11 => {
                let quote = &icmp[8..];
                if quote.len() < 28 || quote[0] >> 4 != 4 {
                    return None;
                }
                let qihl = (quote[0] & 15) as usize * 4;
                if qihl < 20
                    || quote.len() < qihl + 8
                    || quote[9] != 1
                    || quote[12..16] != self.config.local
                    || quote[16..20] != self.probe.target
                    || quote[qihl] != 8
                {
                    return None;
                }
                &quote[qihl..]
            }
            _ => return None,
        };
        (u16::from_be_bytes([original[4], original[5]]) == self.probe.id)
            .then_some(self.probe_owner)
    }
}
fn mask(prefix: u8) -> u32 {
    if prefix == 0 {
        0
    } else {
        u32::MAX << (32 - prefix)
    }
}

// Validate bounds and fragments before interpreting transport fields. A tunnel
// prototype has no reassembler; fragments must not bypass port demultiplexing.
fn ipv4(frame: &[u8]) -> Option<&[u8]> {
    if frame.len() < 34 || frame[12..14] != [8, 0] {
        return None;
    }
    let ip = &frame[14..];
    let ihl = (ip[0] & 15) as usize * 4;
    let total = u16::from_be_bytes([ip[2], ip[3]]) as usize;
    if ip[0] >> 4 != 4
        || ihl < 20
        || total < ihl
        || total > ip.len()
        || ip[6] & 0xbf != 0
        || ip[7] != 0
    {
        return None;
    }
    Some(&ip[..total])
}

#[cfg(not(test))]
static mut STATE: State = State::empty();

#[cfg(not(test))]
pub unsafe fn attach(pid: usize, config: Config) -> bool {
    #[expect(static_mut_refs)]
    STATE.attach(pid, config)
}
#[cfg(not(test))]
pub unsafe fn detach(pid: usize) -> bool {
    #[expect(static_mut_refs)]
    STATE.detach(pid)
}
#[cfg(not(test))]
pub unsafe fn owner() -> Option<usize> {
    (STATE.owner != NONE).then_some(STATE.owner)
}
#[cfg(not(test))]
pub unsafe fn udp_owner(frame: &[u8]) -> Option<usize> {
    STATE.udp_owner(frame)
}
#[cfg(not(test))]
pub unsafe fn is_local(ip: [u8; 4]) -> bool {
    STATE.owner != NONE && STATE.config.local == ip
}
#[cfg(not(test))]
pub unsafe fn reject_nic(frame: &[u8]) -> bool {
    STATE.reject_nic(frame)
}
#[cfg(not(test))]
pub unsafe fn inject(pid: usize, frame: &[u8]) -> bool {
    if !STATE.accepts(pid, frame) {
        return false;
    }
    if frame[23] == 1 {
        STATE
            .icmp_owner(frame)
            .is_none_or(|owner| crate::net::netdrv::queue_to(owner, frame))
    } else {
        crate::net::netdrv::deliver(frame)
    }
}
#[cfg(not(test))]
pub unsafe fn claim_probe(pid: usize, target: [u8; 4]) -> Option<Probe> {
    #[expect(static_mut_refs)]
    STATE.claim_probe(pid, target, crate::time::acpi::get_tick_count() as u16)
}
#[cfg(not(test))]
pub unsafe fn release_probe(pid: usize) -> bool {
    if STATE.probe_owner != pid {
        return false;
    }
    STATE.probe_owner = NONE;
    true
}
#[cfg(not(test))]
pub unsafe fn transmit(frame: &[u8], pid: usize) -> Option<bool> {
    // A full queue is an error to the sender, never plaintext fallback to NIC.
    STATE
        .outbound(frame, pid)
        .then(|| crate::net::netdrv::queue_to(STATE.owner, frame))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn config() -> Config {
        Config {
            local: [10, 77, 0, 1],
            port: 51820,
            mtu: 1420,
            count: 2,
            reserved: [0; 3],
            routes: {
                let mut r = [EMPTY_ROUTE; ROUTES_MAX];
                r[0] = Route {
                    network: [10, 77, 0, 2],
                    prefix: 32,
                    reserved: [0; 3],
                };
                r[1] = Route {
                    network: [10, 4, 6, 0],
                    prefix: 24,
                    reserved: [0; 3],
                };
                r
            },
        }
    }
    fn frame(proto: u8, dst: [u8; 4]) -> [u8; 54] {
        let mut f = [0; 54];
        f[12..14].copy_from_slice(&[8, 0]);
        f[14] = 0x45;
        f[16..18].copy_from_slice(&40u16.to_be_bytes());
        f[23] = proto;
        f[26..30].copy_from_slice(&[10, 77, 0, 2]);
        f[30..34].copy_from_slice(&dst);
        f[36..38].copy_from_slice(&51820u16.to_be_bytes());
        f
    }
    #[test]
    fn ownership_and_exit() {
        let mut s = State::empty();
        assert!(s.attach(4, config()));
        assert!(!s.attach(5, config()));
        assert!(!s.detach(5));
        assert!(s.detach(4));
        assert!(s.attach(5, config()));
        assert_eq!(core::mem::size_of::<Config>(), 140);
        assert_eq!(core::mem::size_of::<Probe>(), 12);
    }
    #[test]
    fn udp_only_and_bounds() {
        let mut s = State::empty();
        s.attach(4, config());
        let mut f = frame(17, [10, 3, 4, 2]);
        assert_eq!(s.udp_owner(&f), Some(4));
        f[26..30].copy_from_slice(&config().local);
        assert_eq!(s.udp_owner(&f), None); // NIC input cannot impersonate queued tunnel output
        f[26..30].copy_from_slice(&[10, 77, 0, 2]);
        f[30..34].copy_from_slice(&config().local);
        assert_eq!(s.udp_owner(&f), None);
        f[30..34].copy_from_slice(&[10, 3, 4, 2]);
        f[23] = 6;
        assert_eq!(s.udp_owner(&f), None);
        f[23] = 17;
        f[20] = 0x20;
        assert_eq!(s.udp_owner(&f), None);
        f[20] = 0;
        f[16..18].copy_from_slice(&100u16.to_be_bytes());
        assert_eq!(s.udp_owner(&f), None);
        assert_eq!(s.udp_owner(&f[..30]), None);
    }
    #[test]
    fn return_route_and_injection() {
        let mut s = State::empty();
        s.attach(4, config());
        let mut f = frame(6, [10, 4, 6, 68]);
        f[26..30].copy_from_slice(&config().local);
        assert!(s.outbound(&f, 8));
        assert!(!s.outbound(&f, 4));
        let mut fragment = f;
        fragment[20] = 0x20;
        assert!(s.outbound(&fragment, 8)); // malformed/fragmented packets cannot escape in plaintext
        let mut f = frame(6, config().local);
        assert!(s.accepts(4, &f));
        assert!(!s.accepts(8, &f));
        f[26] = 11;
        assert!(!s.accepts(4, &f));
        f[26] = 10;
        f[23] = 17;
        assert!(!s.accepts(4, &f));
        s.detach(4);
        assert!(!s.outbound(&f, 8));
        assert!(!s.accepts(4, &f));
    }
    #[test]
    fn bad_config() {
        let mut s = State::empty();
        let mut c = config();
        c.count = 0;
        assert!(!s.attach(4, c));
        c = config();
        c.local[0] = 127;
        assert!(!s.attach(4, c));
        c = config();
        c.port = 0;
        assert!(!s.attach(4, c));
        c = config();
        c.mtu = 1500;
        assert!(!s.attach(4, c));
        c = config();
        c.count = 17;
        assert!(!s.attach(4, c));
        c = config();
        c.routes[0].prefix = 33;
        assert!(!s.attach(4, c));
        c = config();
        c.routes[1].network[3] = 68;
        assert!(!s.attach(4, c));
    }
    #[test]
    fn default_route_preserves_physical_traffic() {
        let mut s = State::empty();
        let mut c = config();
        c.count = 1;
        c.routes[0] = EMPTY_ROUTE;
        assert!(s.attach(4, c));
        let mut f = frame(17, [203, 0, 113, 9]);
        f[26..30].copy_from_slice(&[10, 3, 4, 2]);
        assert!(!s.outbound(&f, 8));
        assert!(!s.reject_nic(&f));
        f[26..30].copy_from_slice(&c.local);
        assert!(s.outbound(&f, 8));
        assert!(!s.outbound(&f, 4));
        assert!(s.reject_nic(&f));
        f[26..30].copy_from_slice(&[203, 0, 113, 9]);
        f[30..34].copy_from_slice(&c.local);
        assert!(s.reject_nic(&f));
    }
    #[test]
    fn probe_ownership_and_authenticated_replies() {
        let mut s = State::empty();
        assert!(s.claim_probe(8, [10, 4, 6, 68], 123).is_none());
        assert!(s.attach(4, config()));
        assert!(s.claim_probe(NONE, [10, 4, 6, 68], 123).is_none());
        assert!(s.claim_probe(8, [10, 4, 7, 68], 123).is_none());
        assert!(s.claim_probe(4, [10, 4, 6, 68], 123).is_none());
        assert!(s.claim_probe(8, config().local, 123).is_none());
        assert!(s.claim_probe(8, [10, 4, 6, 68], 123).is_some());
        assert!(s.claim_probe(9, [10, 4, 6, 68], 456).is_none());
        let mut f = frame(1, config().local);
        f[26..30].copy_from_slice(&[10, 4, 6, 68]);
        f[34..42].copy_from_slice(&[0, 0, 0, 0, 0, 123, 0, 1]);
        assert!(s.accepts(4, &f));
        assert_eq!(s.icmp_owner(&f), Some(8));
        f[39] = 124;
        assert_eq!(s.icmp_owner(&f), None);
        f[39] = 123;
        f[29] = 69;
        assert_eq!(s.icmp_owner(&f), None);
        // Killing the probe client releases just its reservation.
        assert!(!s.detach(8));
        assert_eq!(s.owner, 4);
        assert_eq!(s.icmp_owner(&f), None);
        assert!(s.claim_probe(9, [10, 4, 6, 68], 456).is_some());
        assert!(s.detach(4));
        assert_eq!(s.probe_owner, NONE);
    }
    #[test]
    fn traceroute_matches_quoted_probe() {
        let mut s = State::empty();
        assert!(s.attach(4, config()));
        assert!(s.claim_probe(8, [10, 4, 6, 68], 123).is_some());
        let mut f = [0u8; 70];
        f[..34].copy_from_slice(&frame(1, config().local)[..34]);
        f[16..18].copy_from_slice(&56u16.to_be_bytes());
        f[26..30].copy_from_slice(&[10, 4, 6, 1]);
        f[34] = 11;
        f[42] = 0x45;
        f[51] = 1;
        f[54..58].copy_from_slice(&config().local);
        f[58..62].copy_from_slice(&[10, 4, 6, 68]);
        f[62..70].copy_from_slice(&[8, 0, 0, 0, 0, 123, 0, 1]);
        assert_eq!(s.icmp_owner(&f), Some(8));
        f[34] = 3;
        assert_eq!(s.icmp_owner(&f), Some(8));
        f[61] = 69;
        assert_eq!(s.icmp_owner(&f), None);
        f[61] = 68;
        f[42] = 0x4f; // quoted IHL runs past the available bytes
        assert_eq!(s.icmp_owner(&f), None);
        assert_eq!(s.icmp_owner(&f[..69]), None);
    }
}
