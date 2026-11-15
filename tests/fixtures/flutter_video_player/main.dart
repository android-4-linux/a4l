// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; the video_player plugin's documented API. No GPL-3.0
// sources.
//
// A clip with a sound track through the video_player plugin (media3/ExoPlayer on Android):
// what a4l has to run for "a video plays with sound". Logs the player's state twice a second.
//
// With kScript set below it also pauses, seeks and changes speed during the clip, so that
// picture and sound can be measured after each. The clip's events are at 3.0 s and 5.5 s.
//   at 1.0 s   pause for 1.5 s, then play          -> events at 3.0 and 5.5
//   at 6.5 s   seek to 2.0 s                       -> events at 3.0 and 5.5 again
//   at 6.5 s   seek to 2.0 s at twice the speed    -> the same two, 1.25 s apart
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:video_player/video_player.dart';

// Set to true and rebuild for the scripted run.
const bool kScript = false;

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
      if (kScript) _script();
    }).catchError((Object e) {
      // ignore: avoid_print
      print('ACLVID: FAILED $e');
    });
  }

  Future<void> _script() async {
    var step = 0;
    while (mounted && step < 4) {
      await Future<void>.delayed(const Duration(milliseconds: 20));
      final pos = (await _c.position)?.inMilliseconds ?? 0;
      if (step == 0 && pos >= 1000) {
        await _c.pause();
        // ignore: avoid_print
        print('ACLVID: script: paused at $pos');
        await Future<void>.delayed(const Duration(milliseconds: 1500));
        // ignore: avoid_print
        print('ACLVID: script: play');
        await _c.play();
        step = 1;
      } else if (step == 1 && pos >= 6500) {
        // ignore: avoid_print
        print('ACLVID: script: seek from $pos to 2000');
        await _c.seekTo(const Duration(seconds: 2));
        step = 2;
      } else if (step == 2 && pos >= 6500) {
        // ignore: avoid_print
        print('ACLVID: script: seek from $pos to 2000 at speed 2');
        await _c.setPlaybackSpeed(2);
        await _c.seekTo(const Duration(seconds: 2));
        step = 3;
      } else if (step == 3 && _c.value.isCompleted) {
        // ignore: avoid_print
        print('ACLVID: script: done');
        step = 4;
      }
    }
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
