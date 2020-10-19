#include <OS/Interfaces/IApp.h>

class App : public IApp {
	virtual bool Init() { return true; }
	virtual void Exit() {}

	virtual bool Load() override { return true; }
	virtual void Unload() override {}

	virtual void Update(float deltaTime) override {}
	virtual void Draw() override {}

	virtual const char* GetName() override { return "Sphere Test"; }
};

DEFINE_APPLICATION_MAIN(App)