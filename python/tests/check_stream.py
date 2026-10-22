"""Cross-platform integration check. Requires isolated sim backend, history_frames=4.

Run: PYTHONPATH=python python3 python/tests/check_stream.py --host localhost
"""
import argparse
import json
import time
from andor_emccd.client import AndorEMCCDClient, FrameSubscriber


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', default='localhost')
    parser.add_argument('--control-port', type=int, default=47400)
    args = parser.parse_args()
    kw = dict(host=args.host, control_port=args.control_port, ipc=False)
    with AndorEMCCDClient(**kw, acquire=True) as owner, \
            AndorEMCCDClient(**kw) as fast, AndorEMCCDClient(**kw) as slow:
        assert owner.ping()['simulated'], 'isolated simulated backend required'
        fast.open_stream()
        slow.open_stream()
        settings = dict(trigger_mode='internal', exposure_time_s=0.01,
                        kinetic_cycle_time_s=0.02, hbin=16, vbin=16)
        run = owner.arm(3, settings)
        owner.wait(run['run_id'])
        a, h, first = fast.read_stream()
        assert len(a) == 3 and first['loss']['count'] == 0, first
        assert a[0].shape == (32, 32)
        run = owner.arm(3, settings)
        owner.wait(run['run_id'])
        # Retry an unacknowledged reply after two of its frames were evicted.
        raw, blobs = fast.request_raw('stream_read', {'ack': 0})
        assert raw['result'] == first and len(blobs) == 3
        _, newest, normal = fast.read_stream()
        assert len(newest) == 3 and normal['loss']['count'] == 0
        _, kept, delayed = slow.read_stream()
        loss = delayed['loss']
        assert len(kept) == 4 and loss['count'] == 2, delayed
        assert loss['first_seq'] == h[0]['seq']
        assert loss['last_seq'] == h[1]['seq']
        assert loss['first_timestamp'] == h[0]['timestamp']
        assert loss['last_timestamp'] == h[1]['timestamp']
        _, _, empty = slow.read_stream()
        assert empty['loss']['count'] == 0 and not empty['frames']
        fast.close_stream()
        slow.close_stream()
        sub = FrameSubscriber(**kw, status=False, buffer_frames=2)
        try:
            run = owner.arm(20, settings)
            owner.wait(run['run_id'])
            # Wait for all produced frames to reach the receive ring.
            deadline = time.monotonic() + 3
            while sub.dropped < 18 and time.monotonic() < deadline:
                time.sleep(0.01)
            item = sub.recv(2)
            assert item and item[0] == 'frame'
            delivery = item[1]['delivery']
            assert delivery['server_loss']['count'] == 0, delivery
            assert delivery['client_loss']['count'] == 18, delivery
            assert delivery['client_loss']['last_seq'] - delivery['client_loss']['first_seq'] == 17
            item2 = sub.recv(2)
            assert item2[1]['seq'] == item[1]['seq'] + 1
            assert item2[1]['delivery']['client_loss']['count'] == 0
            assert item2[1]['delivery']['server_loss']['count'] == 0
            assert sub.recv(0.03) is None
        finally:
            sub.close()
        print(json.dumps({'ok': True, 'server_loss': loss,
                          'client_delivery': delivery, 'retry': 'passed'}))


if __name__ == '__main__':
    main()
