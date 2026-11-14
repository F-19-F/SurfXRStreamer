use crate::{CONTROL_PORT, LOCAL_IP};
use surfxr_common::{ConResult, HandleTryAgain, ToCon, anyhow::Result, con_bail};
use surfxr_session::{DscpTos, SocketBufferConfig};
use bincode::config;
use serde::{Serialize, de::DeserializeOwned};
use std::{
    io::{Read, Write},
    marker::PhantomData,
    mem,
    net::{IpAddr, SocketAddr, TcpListener, TcpStream},
    time::{Duration, Instant},
};

// This corresponds to the length of the payload
const FRAMED_PREFIX_LENGTH: usize = mem::size_of::<u32>();
const MAX_CONTROL_PAYLOAD: usize = 2 * 1024 * 1024;

pub fn bind(
    timeout: Duration,
    port: u16,
    dscp: Option<DscpTos>,
    buffer_config: SocketBufferConfig,
) -> Result<TcpListener> {
    let socket = TcpListener::bind((LOCAL_IP, port))?.into();

    crate::set_socket_buffers(&socket, buffer_config).ok();

    crate::set_dscp(&socket, dscp);

    socket.set_read_timeout(Some(timeout))?;

    Ok(socket.into())
}

pub fn accept_from_server(
    listener: &TcpListener,
    server_ip: Option<IpAddr>,
    timeout: Duration,
) -> ConResult<(TcpStream, TcpStream)> {
    // Uses timeout set during bind()
    let (socket, server_address) = listener.accept().handle_try_again()?;

    if let Some(ip) = server_ip
        && server_address.ip() != ip
    {
        con_bail!(
            "Connected to wrong client: Expected: {ip}, Found {}",
            server_address.ip()
        );
    }

    socket.set_read_timeout(Some(timeout)).to_con()?;
    socket.set_nodelay(true).to_con()?;

    Ok((socket.try_clone().to_con()?, socket))
}

pub fn connect_to_client(
    timeout: Duration,
    client_ips: &[IpAddr],
    port: u16,
    buffer_config: SocketBufferConfig,
) -> ConResult<(TcpStream, TcpStream)> {
    let split_timeout = timeout / client_ips.len() as u32;

    let mut res = surfxr_common::try_again();
    for ip in client_ips {
        res = TcpStream::connect_timeout(&SocketAddr::new(*ip, port), split_timeout)
            .handle_try_again();

        if res.is_ok() {
            break;
        }
    }
    let socket = res?.into();

    crate::set_socket_buffers(&socket, buffer_config).ok();
    socket.set_read_timeout(Some(timeout)).to_con()?;

    let socket = TcpStream::from(socket);

    socket.set_nodelay(true).to_con()?;

    Ok((socket.try_clone().to_con()?, socket))
}

fn framed_send<S: Serialize>(
    socket: &mut TcpStream,
    buffer: &mut Vec<u8>,
    packet: &S,
) -> Result<()> {
    buffer.resize(FRAMED_PREFIX_LENGTH, 0);

    let encoded_size = bincode::serde::encode_into_std_write(packet, buffer, config::standard())?;

    buffer[0..FRAMED_PREFIX_LENGTH].copy_from_slice(&(encoded_size as u32).to_le_bytes());

    socket.write_all(buffer)?;

    Ok(())
}

fn framed_recv<R: DeserializeOwned>(
    socket: &mut TcpStream,
    buffer: &mut Vec<u8>,
    recv_cursor: &mut Option<usize>,
    timeout: Duration,
) -> ConResult<R> {
    let deadline = Instant::now() + timeout;

    let recv_cursor_ref = if let Some(cursor) = recv_cursor {
        cursor
    } else {
        let mut payload_size_bytes = [0; FRAMED_PREFIX_LENGTH];

        loop {
            let count = socket.peek(&mut payload_size_bytes).handle_try_again()?;
            if count == 0 { con_bail!("Control peer closed"); }
            if count == FRAMED_PREFIX_LENGTH {
                break;
            } else if Instant::now() > deadline {
                return surfxr_common::try_again();
            }
        }

        let payload_size = u32::from_le_bytes(payload_size_bytes) as usize;
        if payload_size == 0 || payload_size > MAX_CONTROL_PAYLOAD {
            con_bail!("Invalid control payload size");
        }
        let size = FRAMED_PREFIX_LENGTH + payload_size;
        buffer.resize(size, 0);

        recv_cursor.insert(0)
    };

    loop {
        let count = socket
            .read(&mut buffer[*recv_cursor_ref..])
            .handle_try_again()?;
        if count == 0 { con_bail!("Control peer closed"); }
        *recv_cursor_ref += count;

        if *recv_cursor_ref == buffer.len() {
            break;
        } else if Instant::now() > deadline {
            return surfxr_common::try_again();
        }
    }

    let (packet, _) =
        bincode::serde::decode_from_slice(&buffer[FRAMED_PREFIX_LENGTH..], config::standard())
            .to_con()?;

    *recv_cursor = None;

    Ok(packet)
}

pub struct ControlSocketSender<T> {
    pub(crate) inner: TcpStream,
    pub(crate) buffer: Vec<u8>,
    pub(crate) _phantom: PhantomData<T>,
}

impl<S: Serialize> ControlSocketSender<S> {
    pub fn send(&mut self, packet: &S) -> Result<()> {
        framed_send(&mut self.inner, &mut self.buffer, packet)
    }
}

pub struct ControlSocketReceiver<T> {
    pub(crate) inner: TcpStream,
    pub(crate) buffer: Vec<u8>,
    pub(crate) recv_cursor: Option<usize>,
    pub(crate) _phantom: PhantomData<T>,
}

impl<R: DeserializeOwned> ControlSocketReceiver<R> {
    pub fn recv(&mut self, timeout: Duration) -> ConResult<R> {
        framed_recv(
            &mut self.inner,
            &mut self.buffer,
            &mut self.recv_cursor,
            timeout,
        )
    }
}

pub fn get_server_listener(timeout: Duration) -> Result<TcpListener> {
    let listener = bind(timeout, CONTROL_PORT, None, SocketBufferConfig::default())?;

    Ok(listener)
}

// Proto-control-socket that can send and receive any packet. After the split, only the packets of
// the specified types can be exchanged
pub struct ProtoControlSocket {
    pub(crate) inner: TcpStream,
}

pub enum PeerType<'a> {
    AnyClient(Vec<IpAddr>),
    Server(&'a TcpListener),
}

impl ProtoControlSocket {
    pub fn from_accepted(socket: TcpStream, timeout: Duration) -> ConResult<Self> {
        // Winsock can inherit the nonblocking listener mode. Framed reads use
        // bounded blocking I/O, including a StreamReady arriving after config.
        socket.set_nonblocking(false).to_con()?;
        socket.set_read_timeout(Some(timeout)).to_con()?;
        socket.set_write_timeout(Some(timeout)).to_con()?;
        socket.set_nodelay(true).to_con()?;
        Ok(Self { inner: socket })
    }
    pub fn connect_to(timeout: Duration, peer: PeerType<'_>) -> ConResult<(Self, IpAddr)> {
        let socket = match peer {
            PeerType::AnyClient(ips) => {
                connect_to_client(timeout, &ips, CONTROL_PORT, SocketBufferConfig::default())?.0
            }
            PeerType::Server(listener) => accept_from_server(listener, None, timeout)?.0,
        };

        let peer_ip = socket.peer_addr().to_con()?.ip();

        Ok((Self { inner: socket }, peer_ip))
    }

    pub fn send<S: Serialize>(&mut self, packet: &S) -> Result<()> {
        framed_send(&mut self.inner, &mut vec![], packet)
    }

    pub fn recv<R: DeserializeOwned>(&mut self, timeout: Duration) -> ConResult<R> {
        framed_recv(&mut self.inner, &mut vec![], &mut None, timeout)
    }

    pub fn split<S: Serialize, R: DeserializeOwned>(
        self,
        timeout: Duration,
    ) -> Result<(ControlSocketSender<S>, ControlSocketReceiver<R>)> {
        self.inner.set_read_timeout(Some(timeout))?;
        self.inner.set_write_timeout(Some(timeout))?;

        Ok((
            ControlSocketSender {
                inner: self.inner.try_clone()?,
                buffer: vec![0; FRAMED_PREFIX_LENGTH],
                _phantom: PhantomData,
            },
            ControlSocketReceiver {
                inner: self.inner,
                buffer: vec![0; FRAMED_PREFIX_LENGTH],
                recv_cursor: None,
                _phantom: PhantomData,
            },
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::Shutdown;
    #[test]
    fn incoming_control_waits_for_a_delayed_packet_from_nonblocking_accept() {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let writer = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let accepted = listener.accept().unwrap().0;
        // Exercise the Winsock inherited mode on all test platforms.
        accepted.set_nonblocking(true).unwrap();
        let timeout = Duration::from_secs(1);
        let mut reader = ProtoControlSocket::from_accepted(accepted, timeout).ok().unwrap();
        let sender = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(60));
            let mut writer = ProtoControlSocket::from_accepted(writer, timeout).ok().unwrap();
            writer.send(&"StreamReady").unwrap();
        });
        assert_eq!(reader.recv::<String>(timeout).ok().unwrap(), "StreamReady");
        sender.join().unwrap();
    }
    #[test]
    fn incoming_control_bounds_payload_and_detects_eof_without_spinning() {
        for payload in [vec![], vec![1, 0], u32::MAX.to_le_bytes().to_vec()] {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let mut writer = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
            let mut reader = ProtoControlSocket::from_accepted(listener.accept().unwrap().0,
                Duration::from_millis(100)).ok().unwrap();
            writer.write_all(&payload).unwrap(); writer.shutdown(Shutdown::Write).unwrap();
            assert!(reader.recv::<String>(Duration::from_millis(100)).is_err());
        }
    }
}
