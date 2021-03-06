/*
 * Copyright (c) 2009, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "camera_display_save.h"
#include "rviz/visualization_manager.h"
#include "rviz/render_panel.h"
#include "rviz/properties/property.h"
#include "rviz/properties/property_manager.h"
#include "rviz/common.h"
#include "rviz/window_manager_interface.h"
#include "rviz/frame_manager.h"
#include "rviz/validate_floats.h"

#include <tf/transform_listener.h>

#include <boost/bind.hpp>

#include <ogre_tools/axes.h>

#include <OGRE/OgreSceneNode.h>
#include <OGRE/OgreSceneManager.h>
#include <OGRE/OgreRectangle2D.h>
#include <OGRE/OgreMaterialManager.h>
#include <OGRE/OgreTextureManager.h>
#include <OGRE/OgreViewport.h>
#include <OGRE/OgreRenderWindow.h>
#include <OGRE/OgreManualObject.h>
#include <OGRE/OgreRoot.h>
#include <OGRE/OgreRenderSystem.h>

#include <wx/frame.h>

using namespace rviz;

namespace articulation_rviz_plugin
{

CameraDisplaySave::RenderListener::RenderListener(CameraDisplaySave* display)
: display_(display)
{

}

void CameraDisplaySave::RenderListener::preRenderTargetUpdate(const Ogre::RenderTargetEvent& evt)
{
  Ogre::Pass* pass = display_->material_->getTechnique(0)->getPass(0);

  if (pass->getNumTextureUnitStates() > 0)
  {
    Ogre::TextureUnitState* tex_unit = pass->getTextureUnitState(0);
    tex_unit->setAlphaOperation( Ogre::LBX_MODULATE, Ogre::LBS_MANUAL, Ogre::LBS_CURRENT, display_->alpha_ );
  }
  else
  {
    display_->material_->setAmbient(Ogre::ColourValue(0.0f, 1.0f, 1.0f, display_->alpha_));
    display_->material_->setDiffuse(Ogre::ColourValue(0.0f, 1.0f, 1.0f, display_->alpha_));
  }
}

void CameraDisplaySave::RenderListener::postRenderTargetUpdate(const Ogre::RenderTargetEvent& evt)
{
  Ogre::Pass* pass = display_->material_->getTechnique(0)->getPass(0);

  if (pass->getNumTextureUnitStates() > 0)
  {
    Ogre::TextureUnitState* tex_unit = pass->getTextureUnitState(0);
    tex_unit->setAlphaOperation( Ogre::LBX_SOURCE1, Ogre::LBS_MANUAL, Ogre::LBS_CURRENT, 0.0f );
  }
  else
  {
    display_->material_->setAmbient(Ogre::ColourValue(0.0f, 1.0f, 1.0f, 0.0f));
    display_->material_->setDiffuse(Ogre::ColourValue(0.0f, 1.0f, 1.0f, 0.0f));
  }
 
  //display_->render_panel_->getRenderWindow()->setAutoUpdated(false);
}

CameraDisplaySave::CameraDisplaySave( const std::string& name, VisualizationManager* manager )
: Display( name, manager )
, transport_("raw")
, caminfo_tf_filter_(*manager->getTFClient(), "", 2, update_nh_)
, new_caminfo_(false)
, texture_(update_nh_)
, frame_(0)
, force_render_(false)
, render_listener_(this)
{
  scene_node_ = scene_manager_->getRootSceneNode()->createChildSceneNode();

  {
    static int count = 0;
    std::stringstream ss;
    ss << "CameraDisplaySaveObject" << count++;

    screen_rect_ = new Ogre::Rectangle2D(true);
    screen_rect_->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY - 1);
    screen_rect_->setCorners(-1.0f, 1.0f, 1.0f, -1.0f);

    ss << "Material";
    material_ = Ogre::MaterialManager::getSingleton().create( ss.str(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME );
    material_->setSceneBlending( Ogre::SBT_TRANSPARENT_ALPHA );
    material_->setDepthWriteEnabled(false);

    material_->setReceiveShadows(false);
    material_->setDepthCheckEnabled(false);


#if 1
    material_->getTechnique(0)->setLightingEnabled(false);
    Ogre::TextureUnitState* tu = material_->getTechnique(0)->getPass(0)->createTextureUnitState();
    tu->setTextureName(texture_.getTexture()->getName());
    tu->setTextureFiltering( Ogre::TFO_NONE );
    tu->setAlphaOperation( Ogre::LBX_SOURCE1, Ogre::LBS_MANUAL, Ogre::LBS_CURRENT, 0.0 );
#else
    material_->getTechnique(0)->setLightingEnabled(true);
    material_->setAmbient(Ogre::ColourValue(0.0f, 1.0f, 1.0f, 1.0f));
#endif

    material_->setCullingMode(Ogre::CULL_NONE);
    Ogre::AxisAlignedBox aabInf;
    aabInf.setInfinite();
    screen_rect_->setBoundingBox(aabInf);
    screen_rect_->setMaterial(material_->getName());
    scene_node_->attachObject(screen_rect_);

  }

  setAlpha( 0.5f );

  wxWindow* parent = 0;

  WindowManagerInterface* wm = vis_manager_->getWindowManager();
  if (wm)
  {
    parent = wm->getParentWindow();
  }
  else
  {
    frame_ = new wxFrame(0, wxID_ANY, wxString::FromAscii(name.c_str()), wxPoint(100,100), wxDefaultSize, wxMINIMIZE_BOX | wxMAXIMIZE_BOX | wxRESIZE_BORDER | wxCAPTION | wxCLIP_CHILDREN);
    parent = frame_;
  }

  render_panel_ = new RenderPanel(parent, false);
  render_panel_->SetSize(wxSize(640, 480));
  if (wm)
  {
    wm->addPane(name, render_panel_);
  }

  render_panel_->createRenderWindow();
  render_panel_->initialize(vis_manager_->getSceneManager(), vis_manager_);

  render_panel_->setAutoRender(false);
  render_panel_->getRenderWindow()->addListener(&render_listener_);
  render_panel_->getViewport()->setOverlaysEnabled(false);
  render_panel_->getViewport()->setClearEveryFrame(true);
  render_panel_->getRenderWindow()->setActive(false);
  render_panel_->getRenderWindow()->setAutoUpdated(false);
  render_panel_->getCamera()->setNearClipDistance( 0.1f );

  caminfo_tf_filter_.connectInput(caminfo_sub_);
  caminfo_tf_filter_.registerCallback(boost::bind(&CameraDisplaySave::caminfoCallback, this, _1));
  vis_manager_->getFrameManager()->registerFilterForTransformStatusCheck(caminfo_tf_filter_, this);
}

CameraDisplaySave::~CameraDisplaySave()
{
  unsubscribe();
  caminfo_tf_filter_.clear();

  if (frame_)
  {
    frame_->Destroy();
  }
  else
  {
    WindowManagerInterface* wm = vis_manager_->getWindowManager();
    wm->removePane(render_panel_);
    render_panel_->Destroy();
  }

  delete screen_rect_;

  scene_node_->getParentSceneNode()->removeAndDestroyChild(scene_node_->getName());
}

void CameraDisplaySave::onEnable()
{
  subscribe();

  if (frame_)
  {
    frame_->Show(true);
  }
  else
  {
    WindowManagerInterface* wm = vis_manager_->getWindowManager();
    wm->showPane(render_panel_);
  }

  render_panel_->getRenderWindow()->setActive(true);
}

void CameraDisplaySave::onDisable()
{
  render_panel_->getRenderWindow()->setActive(false);

  if (frame_)
  {
    frame_->Show(false);
  }
  else
  {
    WindowManagerInterface* wm = vis_manager_->getWindowManager();
    wm->closePane(render_panel_);
  }

  unsubscribe();

  clear();
}

void CameraDisplaySave::subscribe()
{
  if ( !isEnabled() )
  {
    return;
  }

  texture_.setTopic(topic_);

  // parse out the namespace from the topic so we can subscribe to the caminfo
  std::string caminfo_topic = "camera_info";
  size_t pos = topic_.rfind('/');
  if (pos != std::string::npos)
  {
    std::string ns = topic_;
    ns.erase(pos);

    caminfo_topic = ns + "/" + caminfo_topic;
  }

  caminfo_sub_.subscribe(update_nh_, caminfo_topic, 1);
}

void CameraDisplaySave::unsubscribe()
{
  texture_.setTopic("");
  caminfo_sub_.unsubscribe();
}

void CameraDisplaySave::setAlpha( float alpha )
{
  alpha_ = alpha;

  propertyChanged(alpha_property_);

  causeRender();
}

void CameraDisplaySave::setTopic( const std::string& topic )
{
  unsubscribe();

  topic_ = topic;
  clear();

  subscribe();

  propertyChanged(topic_property_);
}

void CameraDisplaySave::setTransport(const std::string& transport)
{
  transport_ = transport;

  texture_.setTransportType(transport);

  propertyChanged(transport_property_);
}

void CameraDisplaySave::clear()
{
  texture_.clear();
  force_render_ = true;

  new_caminfo_ = false;
  current_caminfo_.reset();

  setStatus(status_levels::Warn, "CameraInfo", "No CameraInfo received on [" + caminfo_sub_.getTopic() + "].  Topic may not exist.");
  setStatus(status_levels::Warn, "Image", "No Image received");

  render_panel_->getCamera()->setPosition(Ogre::Vector3(999999, 999999, 999999));
}

void CameraDisplaySave::updateStatus()
{
  if (texture_.getImageCount() == 0)
  {
    setStatus(status_levels::Warn, "Image", "No image received");
  }
  else
  {
    std::stringstream ss;
    ss << texture_.getImageCount() << " images received";
    setStatus(status_levels::Ok, "Image", ss.str());
  }
}

void CameraDisplaySave::saveImage(wxString ss) {
	wxWindowDC wdc(render_panel_);
	wxSize s = render_panel_->GetSize() ;
	wxBitmap b(s.x, s.y);

	wxMemoryDC md;
	md.SelectObject(b) ;
	md.Blit(0, 0, s.x, s.y, &wdc, 0, 0) ;
	wxString filename = wxString::Format(wxT("/tmp/camera-%s-%05d.png"),ss.c_str(),texture_.getImageCount());
	if ( !filename.empty() ) {
		b.SaveFile(filename,wxBITMAP_TYPE_PNG);
	}
}

void CameraDisplaySave::update(float wall_dt, float ros_dt)
{
  updateStatus();

  try
  {
    if (texture_.update() || force_render_)
    {
      float old_alpha = alpha_;
      if (texture_.getImageCount() == 0)
      {
        alpha_ = 1.0f;
      }

//      if(alpha_ != 0.49) {
//		  updateCamera();
//		  render_panel_->getRenderWindow()->update();
//      } else {
		  alpha_ = 0.0;
		  updateCamera();
		  render_panel_->getRenderWindow()->update();

		  // copy model image into md
			wxWindowDC wdc(render_panel_);
			wxSize s = render_panel_->GetSize() ;
			wxBitmap b(s.x, s.y,24);

			wxMemoryDC md;
			md.SelectObject(b) ;
			md.Blit(0, 0, s.x, s.y, &wdc, 0, 0) ;

			wxColour c;
			md.GetPixel(1,1,&c);
//			b.SetMask(new wxMask(b,c) );

		  alpha_ = 1.0;
		  updateCamera();
		  render_panel_->getRenderWindow()->update();

		  // copy model image into md
			wxMemoryDC md2;
			wxBitmap b2(s.x, s.y,24);
			md2.SelectObject(b2);
			md2.Blit(0, 0, s.x, s.y, &wdc, 0, 0,wxCOPY,false) ;

			wxMemoryDC md3;
			wxBitmap b3(s.x, s.y,24);

			wxImage A = b.ConvertToImage();
			wxImage B = b2.ConvertToImage();
			wxImage C = b3.ConvertToImage();

			unsigned char* pA = A.GetData();
			unsigned char* pB = B.GetData();
			unsigned char* pC = C.GetData();

			for(int i=0;i<s.x*s.y;i++) {
				bool img = (pA[0]==0) && (pA[1]==0) && (pA[2]==0);
				if(!img) {
					*(pC++) = *(pA++);
					*(pC++) = *(pA++);
					*(pC++) = *(pA++);
					pB += 3;
				} else {
					*(pC++) = *(pB++);
					*(pC++) = *(pB++);
					*(pC++) = *(pB++);
					pA += 3;
				}
			}

			wxString filename = wxString::Format(wxT("/tmp/camera-%05d.png"),texture_.getImageCount());
			if ( !filename.empty() ) {
				std::cout << "saving"<<filename.mb_str()<<std::endl;
				C.SaveFile(filename,wxBITMAP_TYPE_PNG);
			}
//      }

      alpha_ = old_alpha;

      force_render_ = false;
    }
  }
  catch (UnsupportedImageEncoding& e)
  {
    setStatus(status_levels::Error, "Image", e.what());
  }
}

void CameraDisplaySave::updateCamera()
{
  sensor_msgs::CameraInfo::ConstPtr info;
  sensor_msgs::Image::ConstPtr image;
  {
    boost::mutex::scoped_lock lock(caminfo_mutex_);

    info = current_caminfo_;
    image = texture_.getImage();
  }

  if (!info || !image)
  {
    return;
  }

  Ogre::Vector3 position;
  Ogre::Quaternion orientation;
  vis_manager_->getFrameManager()->getTransform(image->header, position, orientation, false);

  // convert vision (Z-forward) frame to ogre frame (Z-out)
  orientation = orientation * Ogre::Quaternion(Ogre::Degree(180), Ogre::Vector3::UNIT_X);

  float width = info->width;
  float height = info->height;

  // If the image width is 0 due to a malformed caminfo, try to grab the width from the image.
  if (info->width == 0)
  {
    ROS_DEBUG("Malformed CameraInfo on camera [%s], width = 0", getName().c_str());

    width = texture_.getWidth();
  }

  if (info->height == 0)
  {
    ROS_DEBUG("Malformed CameraInfo on camera [%s], height = 0", getName().c_str());

    height = texture_.getHeight();
  }

  if (height == 0.0 || width == 0.0)
  {
    setStatus(status_levels::Error, "CameraInfo", "Could not determine width/height of image due to malformed CameraInfo (either width or height is 0)");
    return;
  }

  double fx = info->P[0];
  double fy = info->P[5];
  double fovy = 2*atan(height / (2 * fy));
  double aspect_ratio = width / height;
//  if (!validateFloats(fovy))
  {
    setStatus(status_levels::Error, "CameraInfo", "CameraInfo/P resulted in an invalid fov calculation (nans or infs)");
    return;
  }

  // Add the camera's translation relative to the left camera (from P[3]);
  // Tx = -1*(P[3] / P[0])
  double tx = -1 * (info->P[3] / fx);
  Ogre::Vector3 right = orientation * Ogre::Vector3::UNIT_X;
  position = position + (right * tx);

//  if (!validateFloats(position))
  {
    setStatus(status_levels::Error, "CameraInfo", "CameraInfo/P resulted in an invalid position calculation (nans or infs)");
    return;
  }

  double cx = info->P[2];
  double cy = info->P[6];
  double normalized_cx = cx / width;
  double normalized_cy = cy / height;
  double dx = 2*(0.5 - normalized_cx);
  double dy = 2*(normalized_cy - 0.5);

  render_panel_->getCamera()->setFOVy(Ogre::Radian(fovy));
  render_panel_->getCamera()->setAspectRatio(aspect_ratio);
  render_panel_->getCamera()->setPosition(position);
  render_panel_->getCamera()->setOrientation(orientation);
  screen_rect_->setCorners(-1.0f + dx, 1.0f + dy, 1.0f + dx, -1.0f + dy);

  setStatus(status_levels::Ok, "CameraInfo", "OK");

#if 0
  static ogre_tools::Axes* debug_axes = new ogre_tools::Axes(scene_manager_, 0, 0.2, 0.01);
  debug_axes->setPosition(position);
  debug_axes->setOrientation(orientation);
#endif
}

void CameraDisplaySave::caminfoCallback(const sensor_msgs::CameraInfo::ConstPtr& msg)
{
  boost::mutex::scoped_lock lock(caminfo_mutex_);
  current_caminfo_ = msg;
  new_caminfo_ = true;
}

void CameraDisplaySave::onTransportEnumOptions(V_string& choices)
{
  texture_.getAvailableTransportTypes(choices);
}

void CameraDisplaySave::createProperties()
{
  topic_property_ = property_manager_->createProperty<ROSTopicStringProperty>( "Image Topic", property_prefix_, boost::bind( &CameraDisplaySave::getTopic, this ),
                                                                         boost::bind( &CameraDisplaySave::setTopic, this, _1 ), parent_category_, this );
  setPropertyHelpText(topic_property_, "sensor_msgs::Image topic to subscribe to.  The topic must be a well-formed <strong>camera</strong> topic, and in order to work properly must have a matching <strong>camera_info<strong> topic.");
  ROSTopicStringPropertyPtr topic_prop = topic_property_.lock();
  topic_prop->setMessageType(ros::message_traits::datatype<sensor_msgs::Image>());

  alpha_property_ = property_manager_->createProperty<FloatProperty>( "Alpha", property_prefix_, boost::bind( &CameraDisplaySave::getAlpha, this ),
                                                                      boost::bind( &CameraDisplaySave::setAlpha, this, _1 ), parent_category_, this );
  setPropertyHelpText(alpha_property_, "The amount of transparency to apply to the camera image.");

  transport_property_ = property_manager_->createProperty<EditEnumProperty>("Transport Hint", property_prefix_, boost::bind(&CameraDisplaySave::getTransport, this),
                                                                            boost::bind(&CameraDisplaySave::setTransport, this, _1), parent_category_, this);
  EditEnumPropertyPtr ee_prop = transport_property_.lock();
  ee_prop->setOptionCallback(boost::bind(&CameraDisplaySave::onTransportEnumOptions, this, _1));
}

void CameraDisplaySave::fixedFrameChanged()
{
  caminfo_tf_filter_.setTargetFrame(fixed_frame_);
  texture_.setFrame(fixed_frame_, vis_manager_->getTFClient());
}

void CameraDisplaySave::targetFrameChanged()
{

}

void CameraDisplaySave::reset()
{
  Display::reset();

  clear();
}

} // namespace rviz
