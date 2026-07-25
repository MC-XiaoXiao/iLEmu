#pragma once

namespace ilemu {

class UserlandHleRegistry;

class AudioToolboxHle {
public:
  explicit AudioToolboxHle(UserlandHleRegistry &registry);

private:
  void play_system_sound(class UserlandHleCall &call);
};

} // namespace ilemu
