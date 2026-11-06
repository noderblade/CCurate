#pragma once

namespace raven
{
	class RApplication {
	public:
		void RApplication();
		void ~RApplication();
		
		void Exec();
		
		virtual void Tick();
	};
	
	class RWindow {
	public:
		RWindow(const char* title, int width, int height);
		virtual ~RWindow();
	};
	
	class RWidget {
		RWidget();
		virtual ~RWidget();
	};
	
	class RLabel : public RWidget {
		
	};
}