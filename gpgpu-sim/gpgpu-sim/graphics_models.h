// Copyright (c) 2018, Ayub A. Gubran, Tor M. Aamodt
// The University of British Columbia
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice, this
// list of conditions and the following disclaimer in the documentation and/or
// other materials provided with the distribution.
// Neither the name of The University of British Columbia nor the names of its
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef FIXED_GRAPHICS_PIPELINE_H
#define FIXED_GRAPHICS_PIPELINE_H

#include <vector>
#include <list>
#include <utility>
#include <algorithm>
#include <deque>
#include "graphics/mesa_gpgpusim.h"

#include "delayqueue.h"
#include "shader.h"


extern renderData_t g_renderData;

class tc_engine_t {
   static unsigned tc_engine_id_count;
   unsigned m_tc_engine_id;
   struct tc_fragment_t {
      tc_fragment_t(): fragment(NULL)
      {}
      RasterTile::rasterFragment_t* fragment;
   };

   struct tc_fragment_quad_t {
      tc_fragment_quad_t(): fragments(QUAD_SIZE)
      {
         reset();
      }
      void reset(){
         covered=false;
         for(unsigned f=0; f<fragments.size(); f++){
            fragments[f].fragment = NULL;
         }
      }
      bool covered;
      std::vector<tc_fragment_t> fragments;
   };

   public:
   tc_engine_t(unsigned tc_tile_h, unsigned tc_tile_w,
         unsigned r_tile_h, unsigned r_tile_w, 
         unsigned wait_threshold): 
      m_tc_tile_h(tc_tile_h), m_tc_tile_w(tc_tile_w),
      m_r_tiles_count(tc_tile_h*tc_tile_w),
      m_r_tile_h(r_tile_h), m_r_tile_w(r_tile_w),
      m_r_tile_size(r_tile_h*r_tile_w),
      m_wait_threshold(wait_threshold)
   {
      m_tc_engine_id=tc_engine_id_count++;
      //raster tiles should be made out of quads
      assert(m_r_tile_h%2 == 0 and m_r_tile_w%2 == 0);
      m_afragments.resize(m_r_tiles_count, 
            std::vector <tc_fragment_quad_t>(m_r_tile_size/QUAD_SIZE));
      m_status.reset();
   }

   void set_current_coords(unsigned x, unsigned y){
      //only possible to (re)assign empty tiles
      assert(m_input_tiles_bin.size() == 0);
      x = x - x%m_tc_tile_w;
      y = y - y%m_tc_tile_h;
      m_status.rtile_xstart = x;
      m_status.rtile_xend = x + m_tc_tile_w - 1;
      m_status.rtile_ystart = y;
      m_status.rtile_yend = y + m_tc_tile_h - 1;
   }

   //check if raster tile is mapped to this bin
   bool has_tile(unsigned x, unsigned y){
      if(       x >= m_status.rtile_xstart 
            and x <= m_status.rtile_xend
            and y >= m_status.rtile_ystart
            and y <= m_status.rtile_yend)
         return true;
      return false;
   }

   bool empty(){
      return ((m_input_tiles_bin.size() == 0) and
         (m_status.pending_frags == 0));
   }

   bool append_tile(RasterTile* tile){
      assert(has_tile(tile->xCoord, tile->yCoord));
      if(m_input_tiles_bin.size() < m_r_tiles_count){
         m_input_tiles_bin.push_back(tile);
         return true;
      }
      m_status.pending_flush = true;
      return false;
   }

   bool insert_first_tile(RasterTile* tile){
      if(m_input_tiles_bin.size() == 0 and m_status.pending_frags==0){
         set_current_coords(tile->xCoord, tile->yCoord);
         m_input_tiles_bin.push_back(tile);
         m_status.reset();
         return true;
      }
      return false;
   }

   void flush(){
      if(m_status.pending_frags == 0)
         return;
      m_status.waiting_cycles++;
      pending_tiles_t::iterator pending_it = 
         m_pending_tiles.find(std::make_pair(m_status.rtile_xstart,
                  m_status.rtile_ystart));
      if(pending_it != m_pending_tiles.end()){
         tcTilePtr_t pending = pending_it->second;
         if(pending->done){
            m_pending_tiles.erase(std::make_pair(m_status.rtile_xstart, 
                     m_status.rtile_ystart));
         } else return;
      }
      tcTilePtr_t tc_tile = 
         new tcTile_t(m_status.rtile_xstart, m_status.rtile_ystart);
      if(m_status.pending_flush or
            (m_status.waiting_cycles > m_wait_threshold)){
         for(unsigned tileId=0; tileId<m_afragments.size(); tileId++){
            for(unsigned quadId=0; quadId<m_afragments[tileId].size(); quadId++){
               for(unsigned fragId=0; fragId<QUAD_SIZE; fragId++){
                  tc_tile->push_back(
                        m_afragments[tileId][quadId].fragments[fragId].fragment);
               }
               m_afragments[tileId][quadId].reset();
            }
         }

         assert(m_pending_tiles.find(std::make_pair(tc_tile->x,tc_tile->y))
               == m_pending_tiles.end());
         m_pending_tiles.insert(
               std::make_pair(std::make_pair(tc_tile->x, tc_tile->y), tc_tile));
         g_renderData.launchTCTile(tc_tile, m_status.done_prims);
         m_status.reset();
      }
   }
   void assemble(){
      //check 1 tile per cycle, may make it configurable later
      std::vector<RasterTile*> remove_list; for(std::list<RasterTile*>::iterator it = m_input_tiles_bin.begin(); it!=m_input_tiles_bin.end(); ++it){
         RasterTile* rtile = *it;
         unsigned tc_x = rtile->xCoord%m_tc_tile_w;
         unsigned tc_y = rtile->yCoord%m_tc_tile_h;
         unsigned dstTileId = tc_x*m_tc_tile_w + tc_y;
         for(unsigned quadId=0; quadId<m_afragments[dstTileId].size(); quadId++){
            //check if this quad is already covered
            if(!m_afragments[dstTileId][quadId].covered){
               for(unsigned fragId=0; fragId<QUAD_SIZE; fragId++){
                  RasterTile::rasterFragment_t* frag = 
                     &(rtile->getRasterFragment(quadId, fragId));
                  if(rtile->getActiveCount()>0 and frag->alive){
                     m_afragments[dstTileId][quadId].covered = true;
                     m_afragments[dstTileId][quadId].fragments[fragId].fragment = frag;
                     frag->alive = false;
                     m_status.pending_frags++;
                     if(rtile->decActiveCount() == 0){
                        remove_list.push_back(rtile);
                     }
                  }
               }
            }
         }
      }

      for(unsigned r=0; r<remove_list.size(); r++){
         if(remove_list[r]->lastPrimTile){
            m_status.done_prims++;
         }
         assert((std::find(m_input_tiles_bin.begin(), 
                  m_input_tiles_bin.end(), remove_list[r]) != 
                  m_input_tiles_bin.end()));
         m_input_tiles_bin.remove(remove_list[r]);
      }
      remove_list.clear();
   }
   void cycle(){
      flush();
      assemble();
   }

   private:
   //3d vector tiles, quads, fragments
   std::vector<std::vector <tc_fragment_quad_t>> m_afragments;
   std::list<RasterTile*> m_input_tiles_bin;
   //tc tile size in raster tiles
   const unsigned m_tc_tile_h;
   const unsigned m_tc_tile_w;
   const unsigned m_r_tiles_count;
   //raster tile size in fragments
   const unsigned m_r_tile_h;
   const unsigned m_r_tile_w;
   const unsigned m_r_tile_size;
   typedef std::map<std::pair<unsigned, unsigned>, tcTilePtr_t> pending_tiles_t;
   pending_tiles_t m_pending_tiles;

   struct status_t {
      bool pending_flush;
      unsigned pending_frags;
      unsigned waiting_cycles;
      unsigned done_prims;
      //the current tile coord will range 
      //from (rx_coord, ry_coord) to (rx_coord + m_tc_tile_w, ry_coord + m_tc_tile_h)
      unsigned rtile_xstart; 
      unsigned rtile_xend;
      unsigned rtile_ystart;
      unsigned rtile_yend;
      void reset(){
         pending_flush=false;
         pending_frags=0;
         waiting_cycles=0;
         done_prims=0;
         rtile_xstart=-1;
         rtile_xend=-1;
         rtile_ystart=-1;
         rtile_yend=-1;
      }
   };
   status_t m_status;
   const unsigned m_wait_threshold;
};

class tile_assembly_stage_t {
   public:
   tile_assembly_stage_t(unsigned _tc_bins, 
         unsigned tc_tile_h, unsigned tc_tile_w,
         unsigned r_tile_h, unsigned r_tile_w,
         unsigned wait_threshold): 
      tc_engines(_tc_bins, tc_engine_t(tc_tile_h, tc_tile_w, 
               r_tile_h, r_tile_w, wait_threshold)), 
      tc_bins(_tc_bins)
   {}

   bool insert(RasterTile* tile){
      for(unsigned i=0; i<tc_engines.size(); i++){
         if(tc_engines[i].has_tile(tile->xCoord, tile->yCoord)){
            if(tc_engines[i].append_tile(tile)){
               return true;
            }
         }
      }
      for(unsigned i=0; i<tc_engines.size(); i++){
         if(tc_engines[i].insert_first_tile(tile)){
            return true;
         }
      }
      return false;
   }

   bool empty(){
      bool is_empty = true;
      for(unsigned te=0; te<tc_engines.size(); te++){
         is_empty = is_empty and tc_engines[te].empty();
      }
      return is_empty;
   }

   void cycle(){
      for(unsigned te=0; te<tc_engines.size(); te++){
         tc_engines[te].cycle();
      }
   }
   private:
   std::vector<tc_engine_t> tc_engines;
   const unsigned tc_bins;
};

class graphics_simt_pipeline {
   private:
      struct primitive_data_t {
         primitive_data_t(primitiveFragmentsData_t* _prim, unsigned _delay):
            prim(_prim), delay(_delay)
         {}
         primitiveFragmentsData_t* const prim;
         unsigned delay;
      };
   public:
      graphics_simt_pipeline(unsigned simt_cluster_id,
            unsigned setup_delay, unsigned setup_q_len,
            unsigned c_tiles_per_cycle,
            unsigned f_tiles_per_cycle,
            unsigned pre_z_tiles_per_cycle,
            unsigned hiz_tiles_per_cycle,
            unsigned tc_bins,
            unsigned tc_tile_h, unsigned tc_tile_w,
            unsigned r_tile_h, unsigned r_tile_w,
            unsigned tc_wait_threshold
            ): 
         m_cluster_id(simt_cluster_id),
         m_ta_stage(tc_bins, tc_tile_h, tc_tile_w, 
               r_tile_h, r_tile_w, tc_wait_threshold),
         m_setup_delay(setup_delay),
         m_c_tiles_per_cycle(c_tiles_per_cycle),
         m_f_tiles_per_cycle(f_tiles_per_cycle),
         m_pre_z_tiles_per_cycle(pre_z_tiles_per_cycle),
         m_hiz_tiles_per_cycle(hiz_tiles_per_cycle)
   { 
      m_setup_pipe = new fifo_pipeline<primitive_data_t>("setup-stage", 0, setup_q_len);
      m_c_raster_pipe = new fifo_pipeline<primitive_data_t>("coarse-raster-stage", 0, 2);
      m_hiz_pipe = new fifo_pipeline<RasterTile>("hiz-stage", 0, 5);
      m_f_raster_pipe = new fifo_pipeline<RasterTile>("fine-raster-stage", 0, 5);
      m_pre_z_pipe = new fifo_pipeline<RasterTile>("pre-z-stage", 0, 5);
      m_zunit_pipe = new fifo_pipeline<RasterTile>("zunit-stage", 0, 5);
      m_ta_pipe = new fifo_pipeline<RasterTile>("tile-assembly-stage", 0, 5);
      m_current_c_tile = 0;
   }

      ~graphics_simt_pipeline(){
         delete m_setup_pipe;
         delete m_c_raster_pipe;
         delete m_hiz_pipe;
         delete m_f_raster_pipe;
         delete m_zunit_pipe;
         delete m_ta_pipe;
      }

      void cycle(){
         run_ta_stage();
         run_z_unit();
         run_pre_z();
         run_f_raster();
         run_hiz();
         run_c_raster();
         run_setup();
      }

      void run_setup(){
         primitive_data_t* prim = m_setup_pipe->top();
         if(prim){
            if(prim->delay > 0){
               prim->delay--;
               return;
            }
            if(m_c_raster_pipe->full()) return;
            m_c_raster_pipe->push(prim);
            m_setup_pipe->pop();
         } else {
            m_setup_pipe->pop();
         }
      }

      void run_c_raster(){
         if(m_c_raster_pipe->empty()) return;
         unsigned processed_tiles = 0;    
         primitive_data_t* prim = m_c_raster_pipe->top();
         assert(prim);
         for(unsigned t=m_current_c_tile;
               t< prim->prim->getSimtTiles(m_cluster_id).size(); t++){
            if(m_hiz_pipe->full()) return;
            RasterTile* tile = prim->prim->getSimtTiles(m_cluster_id)[t];
            if(tile->lastPrimTile or (tile->getActiveCount() > 0)){
               m_hiz_pipe->push(tile);
            }
            m_current_c_tile++;
            processed_tiles++;
            if(processed_tiles == m_c_tiles_per_cycle)
               break;
         }
         if(m_current_c_tile == prim->prim->getSimtTiles(m_cluster_id).size()){
            delete  m_c_raster_pipe->top();
            m_c_raster_pipe->pop();
            m_current_c_tile = 0;
         }
      }

      void run_hiz(){
         for(unsigned processed_tiles=0; processed_tiles < m_hiz_tiles_per_cycle;
               processed_tiles++){
            if(m_hiz_pipe->empty()) return;
            if(m_f_raster_pipe->full()) return;
            RasterTile* tile = m_hiz_pipe->top();
            assert(tile);
            assert(tile->lastPrimTile or (tile->getActiveCount() > 0));
            if(tile->lastPrimTile or g_renderData.testHiz(tile)){
               m_f_raster_pipe->push(tile);
            }
            m_hiz_pipe->pop();
         }
      }

      void run_f_raster(){
         for(unsigned processed_tiles=0; processed_tiles < m_f_tiles_per_cycle;
               processed_tiles++){
            if(m_f_raster_pipe->empty()) return;
            if(m_pre_z_pipe->full()) return;
            RasterTile* tile = m_f_raster_pipe->top();
            assert(tile);
            assert(tile->lastPrimTile or (tile->getActiveCount() > 0));
            m_pre_z_pipe->push(tile);
            m_f_raster_pipe->pop();
         }
      }

      void run_pre_z(){
         for(unsigned processed_tiles=0; processed_tiles < m_pre_z_tiles_per_cycle;
               processed_tiles++){
            if(m_pre_z_pipe->empty()) return;
            if(m_zunit_pipe->full()) return;
            RasterTile* tile = m_pre_z_pipe->top();
            assert(tile);
            assert(tile->lastPrimTile or (tile->getActiveCount() > 0));
            if(!tile->skipFineDepth() and !tile->lastPrimTile){
               tile->testHizThresh();
            }
            if(tile->lastPrimTile 
                  or tile->skipFineDepth()
                  or (tile->getActiveCount() > 0)){
               m_zunit_pipe->push(tile);
            }
            m_pre_z_pipe->pop();
         }
      }

      void run_z_unit(){
         if(m_zunit_pipe->empty()) return;
         if(m_ta_pipe->full()) return;
         RasterTile* tile = m_zunit_pipe->top();
         assert(tile);
         assert(tile->lastPrimTile or (tile->getActiveCount() > 0));
         //check skipFineDepth
         if(tile->lastPrimTile or tile->resetActiveCount() > 0){
            m_ta_pipe->push(tile);
         }
         m_zunit_pipe->pop();
      }

      void run_ta_stage(){
         m_ta_stage.cycle();
         if(m_ta_pipe->empty()) return;
         RasterTile* tile = m_ta_pipe->top();
         assert(tile);
         assert(tile->lastPrimTile or (tile->getActiveCount() > 0));
         if(tile->lastPrimTile){
            if(m_ta_stage.empty()){
               g_renderData.launchTCTile(NULL, 1);
               m_ta_pipe->pop();
            }
         } else if(m_ta_stage.insert(tile)){
            m_ta_pipe->pop();
         }
      }

      bool add_primitive(primitiveFragmentsData_t* prim, unsigned ctilesId){
         //this primitive doesn't touch this simt core
         if(prim->getSimtTiles(m_cluster_id).size() == 0)
            return true;
         if(m_setup_pipe->full())
            return false;
         primitive_data_t* prim_data = new primitive_data_t(prim, m_setup_delay);
         m_setup_pipe->push(prim_data);
         return true;
      }

      //return if pipeline not empty
      unsigned get_not_completed(){
         unsigned not_complete = 
            m_setup_pipe->get_n_element() +
            m_c_raster_pipe->get_n_element() +
            m_hiz_pipe->get_n_element() +
            m_f_raster_pipe->get_n_element() +
            m_pre_z_pipe->get_n_element() +
            m_zunit_pipe->get_n_element() +
            m_ta_pipe->get_n_element();
         unsigned ret = not_complete + (m_ta_stage.empty()? 0 : 1);
         return ret; 
      }

   private:
      const unsigned m_cluster_id;
      fifo_pipeline<primitive_data_t>* m_setup_pipe;
      fifo_pipeline<primitive_data_t>* m_c_raster_pipe;
      fifo_pipeline<RasterTile>* m_hiz_pipe;
      fifo_pipeline<RasterTile>* m_f_raster_pipe;
      fifo_pipeline<RasterTile>* m_pre_z_pipe;
      fifo_pipeline<RasterTile>* m_zunit_pipe;
      fifo_pipeline<RasterTile>* m_ta_pipe;
      unsigned m_current_c_tile;
      tile_assembly_stage_t m_ta_stage;

      //performance configs
      const unsigned m_setup_delay;
      const unsigned m_c_tiles_per_cycle;
      const unsigned m_f_tiles_per_cycle;
      const unsigned m_pre_z_tiles_per_cycle;
      const unsigned m_hiz_tiles_per_cycle;

};


#endif /* GRAPHICS_PIPELINE */
