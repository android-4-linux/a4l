// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; the video_player plugin's documented API. No GPL-3.0
// sources.
//
// A clip with a sound track through the video_player plugin (media3/ExoPlayer on Android):
// what a4l has to run for "a video plays with sound". Logs the player's state twice a second.
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:video_player/video_player.dart';

void main() => runApp(const App());

class App extends StatefulWidget {
  const App({super.key});
  @override
  State<App> createState() => _AppState();
}

class _AppState extends State<App> {
  late final VideoPlayerController _c;
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _c = VideoPlayerController.asset('assets/test.mp4');
    _c.initialize().then((_) {
      final v = _c.value;
      // ignore: avoid_print
      print('ACLVID: initialized ${v.size.width}x${v.size.height} '
          'duration ${v.duration.inMilliseconds} ms');
      setState(() {});
      _c.play();
      _timer = Timer.periodic(const Duration(milliseconds: 500), (_) {
        final s = _c.value;
        // ignore: avoid_print
        print('ACLVID: position ${s.position.inMilliseconds} ms playing ${s.isPlaying} '
            'buffering ${s.isBuffering} completed ${s.isCompleted} error ${s.errorDescription}');
      });
    }).catchError((Object e) {
      // ignore: avoid_print
      print('ACLVID: FAILED $e');
    });
  }

  @override
  void dispose() {
    _timer?.cancel();
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      home: Scaffold(
        backgroundColor: const Color(0xFF203040),
        body: Center(
          child: _c.value.isInitialized
              ? AspectRatio(aspectRatio: _c.value.aspectRatio, child: VideoPlayer(_c))
              : const Text('loading', style: TextStyle(color: Colors.white)),
        ),
      ),
    );
  }
}
