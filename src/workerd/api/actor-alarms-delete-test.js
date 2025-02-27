// This test is aimed towards validating a correct deleteAlarm behavior in workerd.
// Currently running alarms cannot be deleted within alarm() handler, but can delete it everywhere
// else.
import * as assert from 'node:assert'

async function timeout(ms) {
  await scheduler.wait(ms);
  throw new Error(`timed out`);
}

export class DurableObjectExample {
  constructor(state) {
    this.state = state;
  }

  async waitForAlarm(scheduledTime, timeoutOverrideMs) {
    let self = this;
    let prom = new Promise((resolve) => {
      self.resolve = resolve;
    });

    let timeMs = scheduledTime.valueOf();
    let timeoutMs = (timeMs - Date.now().valueOf()) + (timeoutOverrideMs ?? 4000);
    try {
      await Promise.race([prom, timeout(timeoutMs)]);
      if (Date.now() < scheduledTime.valueOf()) {
        throw new Error(`Date.now() is before scheduledTime! ${Date.now()} vs ${scheduledTime.valueOf()}`);
      }
    } catch (e) {
      throw new Error(`error waiting for alarm at ${scheduledTime.valueOf()}: ${e}`);
    }


    let alarm = await this.state.storage.getAlarm();
    if (alarm != null) {
      throw new Error(`alarm time not cleared when handler ends. ${alarm}`);
    }
  }

  async alarm() {
    this.state.alarmsTriggered++;
    let time = await this.state.storage.getAlarm();
    if (time) {
      throw new Error(`time not null inside alarm handler ${time}`);
    }
    // Deleting an alarm inside `alarm()` will not have any effect, unless there's another queued alarm
    // already.
    await this.state.storage.deleteAlarm();

    // On the other hand, if we have an alarm queued, it will be deleted. If this is working properly,
    // we'll only have one alarm triggered.
    await this.state.storage.setAlarm(50);
    await this.state.storage.deleteAlarm();

    // All done inside `alarm()`.
    this.resolve();
  }

  async fetch(request) {
    this.state.alarmsTriggered = 0;
    // We set an alarm that will never trigger because it gets deleted before running.
    await this.state.storage.setAlarm(500);
    await this.state.storage.deleteAlarm();

    // We set another alarm that will run in 0.5s to test that deleting an alarm inside its handler
    // does not cause a crash.
    const time = Date.now() + 500;
    await this.state.storage.setAlarm(time);
    assert.equal(await this.state.storage.getAlarm(), time);

    // We should wait for all alarms the run before returning.
    await this.waitForAlarm(time, 3000);

    // We should have ran `alarm()` only once.
    assert.equal(this.state.alarmsTriggered, 1);

    // All done, return "OK" because if we reach this, everything worked as expected.
    return new Response("OK");
  }
}

export default {
  async test(ctrl, env, ctx) {
    let id = env.ns.idFromName("A");
    let obj = env.ns.get(id);
    let res = await obj.fetch("http://foo/test");
    let text = await res.text();
    assert.equal(text, "OK");
  }
}
