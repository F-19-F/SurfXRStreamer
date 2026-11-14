use super::{PathSegment, parse_path, path_to_string};

    fn debug_path(path: &str) -> String {
        format!("{:?}", parse_path(path).unwrap())
    }

    #[test]
    fn parses_names_only() {
        assert_eq!(debug_path("my.element.content"), "[my, element, content]");
    }

    #[test]
    fn parses_index_between_names() {
        let segments = parse_path("my.element[1].content").unwrap();
        assert!(matches!(
            segments.as_slice(),
            [
                PathSegment::Name(a),
                PathSegment::Name(b),
                PathSegment::Index(1),
                PathSegment::Name(c),
            ] if a == "my" && b == "element" && c == "content"
        ));
    }

    #[test]
    fn parses_chained_indices() {
        assert_eq!(debug_path("matrix[1][2]"), "[matrix, [1], [2]]");
        assert_eq!(debug_path("a[0][12][345]"), "[a, [0], [12], [345]]");
    }

    #[test]
    fn accepts_underscores_and_digits_in_names() {
        assert_eq!(debug_path("_priv.x1[0]._2y"), "[_priv, x1, [0], _2y]");
    }

    #[test]
    fn rejects_malformed_paths() {
        for path in [
            "",
            ".foo",
            "foo.",
            "foo..bar",
            ".[1]",
            "foo.[1]",
            "[0].foo",
            "foo[]",
            "foo[",
            "foo[1",
            "foo[1]bar",
            "foo[-1]",
            "foo[1.5]",
            "foo[key]",
            "foo-bar",
            "foo bar",
            "foo.bar!",
            "fo茅",
        ] {
            assert!(
                parse_path(path).is_err(),
                "expected \"{path}\" to be rejected"
            );
        }
    }

    #[test]
    fn rejects_index_overflow() {
        assert!(parse_path("foo[99999999999999999999999999]").is_err());
    }

    #[test]
    fn path_to_string_round_trips() {
        for path in [
            "a",
            "a.b.c",
            "a[0]",
            "a[1][2].b",
            "video.foveated_encoding.strength",
            "headset.controllers.content.gestures[2].joystick_range",
        ] {
            assert_eq!(path_to_string(&parse_path(path).unwrap()), path);
        }
    }

    #[test]
    fn error_points_at_the_failing_column() {
        assert_eq!(
            parse_path("foo..bar").unwrap_err().to_string(),
            "invalid path \"foo..bar\" at column 5: expected a name `[A-Za-z0-9_]+`, found `.`",
        );
        assert_eq!(
            parse_path("foo[1]x").unwrap_err().to_string(),
            "invalid path \"foo[1]x\" at column 7: expected `.` or end of input, found `x`",
        );
        assert_eq!(
            parse_path("foo[]").unwrap_err().to_string(),
            "invalid path \"foo[]\" at column 4: expected `.` or end of input, found `[`",
        );
    }
