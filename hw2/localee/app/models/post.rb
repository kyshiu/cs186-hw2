class Post < ActiveRecord::Base
  # attr_accessible :title, :body
  belongs_to :user
  belongs_to :location
  
  def to_hash
    {
      :id => self.id,
      :text => self.text,
      :user_id => self.user_id,
      :location_id => self.location_id
    }
  end
end
