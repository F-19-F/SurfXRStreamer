    use super::*;
    #[test]
    fn media_feature_requires_fresh_managed_negotiating_host() {
        let mut tickets=HashMap::new();let mut seen=Instant::now()-Duration::from_secs(30);
        let(mut system,mut unified,mut media,mut port)=(false,false,false,19950);
        let exchange=|bytes:&[u8],tickets:&mut HashMap<[u8;32],Ticket>,seen:&mut Instant,
            system:&mut bool,unified:&mut bool,media:&mut bool,port:&mut u16| {
            let listener=TcpListener::bind((Ipv4Addr::LOCALHOST,0)).unwrap();
            let mut client=TcpStream::connect(listener.local_addr().unwrap()).unwrap();client.write_all(bytes).unwrap();
            let(mut stream,_)=listener.accept().unwrap();
            handle(&mut stream,Ipv4Addr::LOCALHOST.into(),tickets,seen,system,unified,media,port,
                &mut|_,_,_|panic!("Feature query must not authorize or issue credentials"),
                &mut|_|panic!("Feature query must not change monitor preferences"), &mut|_|None).unwrap()
        };
        let query=b"ALVRDB01\x09";
        for(fresh,sys,uni,modes,expected)in[(true,true,true,true,1),(false,true,true,true,0),
            (true,false,true,true,0),(true,true,false,true,0),(true,true,true,false,0)] {
            seen=Instant::now()-Duration::from_secs(if fresh{0}else{30});system=sys;unified=uni;media=modes;
            let original=seen;
            assert_eq!(exchange(query,&mut tickets,&mut seen,&mut system,&mut unified,&mut media,&mut port),vec![expected]);
            assert_eq!(seen,original,"Query must not refresh the daemon heartbeat");assert!(tickets.is_empty());
        }
        // Use actual daemon announcement parsing, then downgrade to a legacy
        // managed host. Negotiation support must never remain latched on.
        for flags in [13u8,5] {
            let mut announce=b"ALVRDB01\x03".to_vec();announce.push(flags);announce.extend(19950u16.to_le_bytes());
            assert_eq!(exchange(&announce,&mut tickets,&mut seen,&mut system,&mut unified,&mut media,&mut port),vec![0]);
            assert_eq!(exchange(query,&mut tickets,&mut seen,&mut system,&mut unified,&mut media,&mut port),vec![u8::from(flags&8!=0)]);
        }
    }
    fn exchange(
        bytes: &[u8],
        tickets: &mut HashMap<[u8; 32], Ticket>,
        trusted: bool,
        settings: &mut Settings,
    ) -> Vec<u8> {
        exchange_live(bytes, tickets, trusted, settings).0
    }
    fn exchange_live(
        bytes: &[u8], tickets: &mut HashMap<[u8; 32], Ticket>,
        trusted: bool, settings: &mut Settings,
    ) -> (Vec<u8>, TcpStream) {
        // Reserve private endpoints; do not start a host, capture, service or SteamVR.
        let listener = TcpListener::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        let mut client = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        client.write_all(bytes).unwrap();
        let (mut stream, _) = listener.accept().unwrap();
        let mut seen = Instant::now();
        let mut system = true;
        let mut unified = false;
        let mut media_modes = false;
        let mut port=19950;
        let admission = settings.clone();
        let reply = handle(
            &mut stream,
            Ipv4Addr::LOCALHOST.into(),
            tickets,
            &mut seen,
            &mut system,
            &mut unified,
            &mut media_modes,
            &mut port,
            &mut |name, _, _| {
                assert_eq!(name, "frame-test");
                Some((trusted, admission.clone()))
            },
            &mut |count| {
                settings.extra.frame_desktop.monitor_count = count;
                true
            },
            &mut |bytes| {assert_eq!(bytes.len(),25);Some(vec![7;40])},
        )
        .unwrap();
        (reply, client)
    }
    #[test]
    fn admission_consumption_and_rule_revocation() {
        let mut settings = surfxr_session::SessionConfig::default().to_settings();
        let mut tickets = HashMap::new();
        let mut request = b"ALVRDB01\0\x0aframe-test".to_vec();
        assert_eq!(
            exchange(&request, &mut tickets, false, &mut settings),
            vec![1]
        );
        assert!(tickets.is_empty());
        settings.extra.frame_desktop.enabled = false;
        assert_eq!(
            exchange(&request, &mut tickets, true, &mut settings),
            vec![2]
        );
        settings.extra.frame_desktop.enabled = true;
        let reply = exchange(&request, &mut tickets, true, &mut settings);
        assert_eq!(reply.len(), 70);
        let id: [u8; 32] = reply[33..65].try_into().unwrap();
        request = b"ALVRDB01\x01".to_vec();
        request.extend(id);
        request.extend([127, 0, 0, 1, 1]);
        assert_eq!(exchange(&request, &mut tickets, true, &mut settings), reply);
        assert_eq!(
            exchange(&request, &mut tickets, true, &mut settings),
            vec![2]
        ); // no replay
        let mut monitors = request.clone();
        monitors[8] = 4;
        monitors.push(0);
        assert_eq!(
            exchange(&monitors, &mut tickets, true, &mut settings),
            vec![0, 1]
        );
        *monitors.last_mut().unwrap() = 3;
        assert_eq!(
            exchange(&monitors, &mut tickets, false, &mut settings),
            vec![2]
        );
        assert_eq!(settings.extra.frame_desktop.monitor_count, 1);
        assert_eq!(
            exchange(&monitors, &mut tickets, true, &mut settings),
            vec![0, 3]
        );
        *monitors.last_mut().unwrap() = 0;
        assert_eq!(
            exchange(&monitors, &mut tickets, true, &mut settings),
            vec![0, 3]
        );
        let mut sync=request.clone();sync[8]=10;sync.extend([0;25]);
        assert_eq!(exchange(&sync,&mut tickets,false,&mut settings),vec![2]);
        tickets.get_mut(&id).unwrap().unified=true;
        let response=exchange(&sync,&mut tickets,true,&mut settings);
        assert_eq!(&response[..5],&[0,40,0,0,0]);assert_eq!(&response[5..],&[7;40]);
        tickets.get_mut(&id).unwrap().consumed=false;
        assert_eq!(exchange(&sync,&mut tickets,true,&mut settings),vec![2]);
        tickets.get_mut(&id).unwrap().consumed=true;
        request[8] = 2;
        assert_eq!(
            exchange(&request, &mut tickets, true, &mut settings),
            vec![0]
        );
        assert!(tickets[&id].expires.duration_since(Instant::now()) > Duration::from_secs(19));
        // Model six seconds without broker scheduling, including the same
        // expiry pruning that the production accept loop runs on resumption.
        tickets.get_mut(&id).unwrap().expires -= Duration::from_secs(6);
        tickets.retain(|_, t| t.expires > Instant::now());
        assert_eq!(exchange(&request, &mut tickets, true, &mut settings), vec![0]);
        assert_eq!(
            exchange(&request, &mut tickets, false, &mut settings),
            vec![2]
        );
        settings.extra.frame_desktop.allow_input = false;
        assert_eq!(
            exchange(&request, &mut tickets, true, &mut settings),
            vec![2]
        );
        settings.extra.frame_desktop.allow_input = true;
        settings.extra.frame_desktop.require_system_service = true;
        *request.last_mut().unwrap() = 0;
        assert_eq!(
            exchange(&request, &mut tickets, true, &mut settings),
            vec![2]
        );
        *request.last_mut().unwrap() = 1;
        tickets.get_mut(&id).unwrap().expires = Instant::now() - Duration::from_secs(1);
        assert_eq!(
            exchange(&request, &mut tickets, true, &mut settings),
            vec![2]
        );
    }

    #[test]
    fn session_visibility_modes_watch_revocation_and_disconnect_cleanup() {
        let mut settings = surfxr_session::SessionConfig::default().to_settings();
        settings.extra.frame_desktop.enabled = true;
        let mut tickets = HashMap::new();
        let reply = exchange(b"ALVRDB01\0\x0aframe-test", &mut tickets, true, &mut settings);
        let id: [u8; 32] = reply[33..65].try_into().unwrap();
        let mut request = b"ALVRDB01\x0b".to_vec();
        request.extend(id); request.extend([127, 0, 0, 1, 1]);
        assert_eq!(exchange(&request, &mut tickets, true, &mut settings), vec![2], "Unconsumed ticket cannot watch");
        request[8] = 1;
        assert_eq!(exchange(&request, &mut tickets, true, &mut settings), reply);
        request[8] = 2;
        for (flags, label) in [(3, "Desktop streaming"), (19, "VR streaming"), (1, "Connected (idle)")] {
            *request.last_mut().unwrap() = flags;
            assert_eq!(exchange(&request, &mut tickets, true, &mut settings), vec![0]);
            let session = sessions::snapshot().into_iter().find(|s| s.id == id).unwrap();
            assert_eq!(session.mode, label);
            assert_eq!(session.ip, IpAddr::V4(Ipv4Addr::LOCALHOST));
            assert_eq!(session.hostname, "frame-test");
        }
        request[8] = 11;
        request[41] = 192;
        assert_eq!(exchange(&request, &mut tickets, true, &mut settings), vec![2], "Peer must match ticket");
        request[41] = 127;
        let (reply, mut watch) = exchange_live(&request, &mut tickets, true, &mut settings);
        assert_eq!(reply, vec![0]);
        assert_eq!(exchange(&request, &mut tickets, true, &mut settings), vec![2], "Only one watcher per session");
        watch.set_read_timeout(Some(Duration::from_millis(250))).unwrap();
        sessions::disconnect(&id);
        let mut notification = [0]; watch.read_exact(&mut notification).unwrap();
        assert_eq!(notification, [2], "Explicit revocation does not wait for a heartbeat");
        request[8] = 2;
        assert_eq!(exchange(&request, &mut tickets, true, &mut settings), vec![2]);
        assert!(sessions::snapshot().iter().find(|s| s.id == id).unwrap().disconnecting);
        drop(watch);
        sessions::prune(&mut tickets);
        assert!(!tickets.contains_key(&id));
        assert!(!sessions::snapshot().iter().any(|s| s.id == id));
        // Disconnect affects only that ticket, not trust or later connections.
        assert_eq!(exchange(b"ALVRDB01\0\x0aframe-test", &mut tickets, true, &mut settings).len(), 70);
    }
